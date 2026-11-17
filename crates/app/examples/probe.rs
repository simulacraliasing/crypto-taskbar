use crypto_taskbar::{engine::now, http::Client};
use crypto_taskbar_core::{Coin, Config, PERIODS, Quote, number_at, period_seconds};
use std::sync::atomic::AtomicBool;
fn main() -> anyhow::Result<()> {
    let client = Client::new()?;
    let stop = AtomicBool::new(false);
    let mut config = Config::default();
    let mut sources = vec![
        "binance".to_owned(),
        "okx".to_owned(),
        "coingecko".to_owned(),
    ];
    let mut symbol = "BTCUSDT".to_owned();
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--source" => {
                sources = vec![
                    args.next()
                        .ok_or_else(|| anyhow::anyhow!("--source needs a name"))?,
                ]
            }
            "--symbol" => {
                symbol = crypto_taskbar_core::canonical_pair(
                    &args
                        .next()
                        .ok_or_else(|| anyhow::anyhow!("--symbol needs a pair"))?,
                )
            }
            _ => anyhow::bail!("Unknown argument: {arg}"),
        }
    }
    for source in sources {
        let coin = Coin {
            label: symbol.clone(),
            symbol: symbol.clone(),
            source: source.clone(),
        };
        config.coins = vec![coin.clone()];
        for period in PERIODS {
            if source == "coingecko" && period != "24h" {
                continue;
            }
            config.change_period = period.into();
            let s = config
                .sources
                .get(&source)
                .ok_or_else(|| anyhow::anyhow!("Unknown source: {source}"))?;
            let body = client.get(&config.request_url(&coin, false, 0)?, s, &stop)?;
            let mut quote = Quote::parse(&body, s, &coin, period)?;
            if period != "24h" {
                let now = now() as i64 * 1000;
                let history = client.get(&config.request_url(&coin, true, now)?, s, &stop)?;
                quote.apply_history(&history, s, &coin)?;
                let timestamp = number_at(
                    &history,
                    if source == "binance" || source == "binance-futures" {
                        "/0/0"
                    } else {
                        "/data/0/0"
                    },
                )
                .unwrap_or(0.);
                let expected = (now - period_seconds(period)? * 1000) / 60000 * 60000;
                anyhow::ensure!(
                    (timestamp - expected as f64).abs() < 120000.,
                    "历史 K 线时间不匹配"
                );
            }
            println!(
                "{source} {period} price={:?} change={:?}",
                quote.price, quote.change
            );
            anyhow::ensure!(quote.price.is_some() && quote.change.is_some(), "行情缺失");
        }
    }
    Ok(())
}
