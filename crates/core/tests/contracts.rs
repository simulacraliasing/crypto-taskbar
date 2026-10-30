use crypto_taskbar_core::*;
use serde_json::json;

#[test]
fn defaults_and_roundtrip() {
    let c = Config::default();
    c.validate().unwrap();
    assert_eq!(c.coins[0].symbol, "BTCUSDT");
    assert!(c.show_change);
    assert_eq!(Config::parse(&serde_json::to_vec(&c).unwrap()).unwrap(), c);
}
#[test]
fn migrate_cpp_config() {
    let mut j = serde_json::to_value(Config::default()).unwrap();
    j.as_object_mut().unwrap().remove("changePeriod");
    j["showChange"] = json!(false);
    j["coins"][0]["symbol"] = json!("bitcoin");
    j["coins"][0]["source"] = json!("coingecko");
    j["sources"]["coingecko"]
        .as_object_mut()
        .unwrap()
        .remove("symbolMap");
    j["sources"]["binance"]
        .as_object_mut()
        .unwrap()
        .remove("historyUrl");
    j["sources"]["binance"]
        .as_object_mut()
        .unwrap()
        .remove("historyPricePath");
    let c = Config::parse(&serde_json::to_vec(&j).unwrap()).unwrap();
    assert!(c.show_change);
    assert_eq!(c.coins[0].symbol, "BTCUSDT");
    assert_eq!(c.sources["coingecko"].symbol(&c.coins[0]), "bitcoin");
    assert!(!c.sources["binance"].history_url.is_empty());
}
#[test]
fn configuration_rejects_invalid_data() {
    for (field, value) in [
        ("refreshSeconds", json!(0)),
        ("rotationSeconds", json!(0)),
        ("width", json!(20)),
        ("changePeriod", json!("9h")),
        ("upColor", json!("red")),
        ("theme", json!("unknown")),
        ("coins", json!([])),
    ] {
        let mut j = serde_json::to_value(Config::default()).unwrap();
        j[field] = value;
        assert!(
            Config::parse(&serde_json::to_vec(&j).unwrap()).is_err(),
            "{field}"
        );
    }
    let mut c = Config::default();
    c.coins[0].source = "missing".into();
    assert!(c.validate().is_err());
    assert!(Config::parse(b"{}").is_err());
}
#[test]
fn coin_limits_and_normalization() {
    let mut c = Config::default();
    c.coins = vec![c.coins[0].clone(); 100];
    assert!(c.validate().is_ok());
    c.coins.push(c.coins[0].clone());
    assert!(c.validate().is_err());
    assert_eq!(canonical_pair(" btc/usdt "), "BTCUSDT");
    assert_eq!(canonical_pair("ETH-USDT"), "ETHUSDT");
    assert_eq!(split_pair("BTCFDUSD"), Some(("BTC", "FDUSD")));
}
#[test]
fn batch_urls_and_mapping() {
    let mut c = Config::default();
    for coin in &mut c.coins {
        coin.source = "coingecko".into();
    }
    let url = c.request_url(&c.coins[0], false, 0).unwrap();
    assert!(url.contains("bitcoin,ethereum,solana"));
    assert_eq!(url, c.request_url(&c.coins[1], false, 0).unwrap());
    assert_eq!(c.sources["okx"].symbol(&c.coins[0]), "BTC-USDT");
    assert_eq!(encode("BTC/USDT &"), "BTC%2FUSDT%20%26");
}
#[test]
fn history_timestamp_and_no_wrong_period() {
    let c = Config {
        change_period: "30d".into(),
        ..Config::default()
    };
    assert!(
        c.request_url(&c.coins[0], true, 1800000000000)
            .unwrap()
            .contains("startTime=1797408000000")
    );
    let src = &c.sources["binance"];
    let mut q = Quote::parse(
        &json!({"lastPrice":"110","priceChangePercent":"88"}),
        src,
        &c.coins[0],
        "30d",
    )
    .unwrap();
    assert!(q.change.is_none());
    q.apply_history(&json!([[123, "100"]]), src, &c.coins[0])
        .unwrap();
    assert!((q.change.unwrap() - 10.).abs() < 1e-9);
    assert!(
        q.apply_history(&json!([[123, "0"]]), src, &c.coins[0])
            .is_err()
    );
    let cg = Quote::parse(
        &json!({"bitcoin":{"usd":110,"usd_24h_change":88}}),
        &c.sources["coingecko"],
        &c.coins[0],
        "4h",
    )
    .unwrap();
    assert!(cg.change.is_none());
    assert!(cg.change_error.is_some());
}
#[test]
fn numeric_and_pointer_contracts() {
    for value in [
        json!(null),
        json!(true),
        json!("NaN"),
        json!("1oops"),
        json!(""),
        json!("1 "),
    ] {
        assert!(number_at(&json!({"n":value}), "/n").is_none());
    }
    assert_eq!(number_at(&json!({"n":"0.000001"}), "/n"), Some(0.000001));
    let coin = Coin {
        label: "Test".into(),
        symbol: "BTCUSDT".into(),
        source: "x".into(),
    };
    let mut src = Source {
        price_path: "/{symbol}/price".into(),
        ..Source::default()
    };
    src.symbol_map.insert("BTCUSDT".into(), "a/b~c".into());
    assert_eq!(
        number_at(
            &json!({"a/b~c":{"price":42}}),
            &src.path(&src.price_path, &coin)
        ),
        Some(42.)
    );
    src.price_path = "/~bad".into();
    assert!(src.validate().is_err());
}
#[test]
fn formatting() {
    assert_eq!(format_price(123456.78), "123,456.78");
    assert_eq!(format_price(100.), "100");
    assert_eq!(format_price(0.00001234), "0.00001234");
    assert_eq!(format_price(1e-9), "1e-9");
    assert_eq!(format_change(-0.001), "+0.00%");
    assert_eq!(format_change(-2.19), "-2.19%");
}
#[test]
fn custom_source_colors_and_environment() {
    let mut c = Config {
        up_color: "#CC1133".into(),
        down_color: "#15AA77".into(),
        ..Config::default()
    };
    c.sources
        .get_mut("binance")
        .unwrap()
        .headers
        .insert("Authorization".into(), "Bearer %PRICE_TOKEN%".into());
    let restored = Config::parse(&serde_json::to_vec(&c).unwrap()).unwrap();
    assert_eq!(c, restored);
    assert_eq!(
        expand_header_with("Bearer %TOKEN%", |_| Some("example".into())).unwrap(),
        "Bearer example"
    );
    assert!(expand_header_with("%MISSING%", |_| None).is_err());
    assert!(expand_header_with("%TOKEN%", |_| Some("x\r\nInjected: yes".into())).is_err());
    c.sources
        .get_mut("binance")
        .unwrap()
        .headers
        .insert("Bad:Name".into(), "x".into());
    assert!(c.validate().is_err());
    assert!(validate_url("https://user:secret@example.com/ticker").is_err());
}
#[test]
fn save_keeps_backup_and_invalid_save_keeps_original() {
    let root = std::env::var_os("CT_TEST_DIR")
        .map(std::path::PathBuf::from)
        .unwrap_or_else(std::env::temp_dir)
        .join(format!(
            "crypto-taskbar-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
    let path = root.join("config.json");
    let mut c = Config::default();
    c.save(&path).unwrap();
    let original = std::fs::read(&path).unwrap();
    c.refresh_seconds = 0;
    assert!(c.save(&path).is_err());
    assert_eq!(original, std::fs::read(&path).unwrap());
    c.refresh_seconds = 60;
    c.save(&path).unwrap();
    assert_eq!(
        original,
        std::fs::read(path.with_extension("json.bak")).unwrap()
    );
    assert_eq!(Config::load(&path).unwrap().refresh_seconds, 60);
    let saved = std::fs::read(&path).unwrap();
    c.sources
        .get_mut("binance")
        .unwrap()
        .symbol_map
        .insert("BTCUSDT".into(), "x".repeat(MAX_CONFIG_BYTES as usize));
    assert!(c.save(&path).is_err());
    assert_eq!(saved, std::fs::read(&path).unwrap());
}
