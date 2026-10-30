use crypto_taskbar::{engine::now, http::Client};
use crypto_taskbar_core::{Coin, Config, PERIODS, Quote, number_at, period_seconds};
use std::sync::atomic::AtomicBool;
fn main() -> anyhow::Result<()> {
    let client = Client::new()?;
    let stop = AtomicBool::new(false);
    let mut config = Config::default();
    for source in ["binance", "okx", "coingecko"] {
        let coin = Coin {
            label: "BTC".into(),
            symbol: "BTCUSDT".into(),
            source: source.into(),
        };
        config.coins = vec![coin.clone()];
        for period in PERIODS {
            if source == "coingecko" && period != "24h" {
                continue;
            }
            config.change_period = period.into();
            let s = &config.sources[source];
            let body = client.get(&config.request_url(&coin, false, 0)?, s, &stop)?;
            let mut quote = Quote::parse(&body, s, &coin, period)?;
            if period != "24h" {
                let now = now() as i64 * 1000;
                let history = client.get(&config.request_url(&coin, true, now)?, s, &stop)?;
                quote.apply_history(&history, s, &coin)?;
                let timestamp = number_at(
                    &history,
                    if source == "binance" {
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
