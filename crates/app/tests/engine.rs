use crypto_taskbar::engine::{Snapshot, now, refresh_snapshot};
use crypto_taskbar_core::{Config, Quote};
use serde_json::json;
use std::{cell::Cell, collections::BTreeMap};

#[test]
fn batch_once_preserve_failure_and_recover() {
    let mut c = Config::default();
    for coin in &mut c.coins {
        coin.source = "coingecko".into();
    }
    let mut snapshot = Snapshot {
        quotes: vec![Quote::default(); c.coins.len()],
        config: c,
    };
    let mut retry = BTreeMap::new();
    let calls = Cell::new(0);
    refresh_snapshot(&mut snapshot, &mut retry, &|| false, |_, _| {
        calls.set(calls.get() + 1);
        Ok(
            json!({"bitcoin":{"usd":100,"usd_24h_change":1},"ethereum":{"usd":20,"usd_24h_change":-2},"solana":{"usd":3,"usd_24h_change":0}}),
        )
    });
    assert_eq!(calls.get(), 1);
    assert_eq!(snapshot.quotes[0].price, Some(100.));
    refresh_snapshot(&mut snapshot, &mut retry, &|| false, |_, _| {
        anyhow::bail!("HTTP 503")
    });
    assert_eq!(snapshot.quotes[0].price, Some(100.));
    assert!(snapshot.quotes[0].stale);
    refresh_snapshot(&mut snapshot, &mut retry, &|| false, |_, _| {
        Ok(json!({"bitcoin":{"usd":101}}))
    });
    assert_eq!(snapshot.quotes[0].price, Some(101.));
    assert!(!snapshot.quotes[0].stale);
    assert!(snapshot.quotes[0].error.is_none());
}
#[test]
fn rate_limit_does_not_extend_on_skipped_refresh() {
    let mut c = Config::default();
    c.coins.truncate(1);
    let mut snapshot = Snapshot {
        quotes: vec![Quote::default()],
        config: c,
    };
    let mut retry = BTreeMap::new();
    refresh_snapshot(&mut snapshot, &mut retry, &|| false, |_, _| {
        anyhow::bail!("HTTP 429")
    });
    let deadline = retry["binance"];
    refresh_snapshot(&mut snapshot, &mut retry, &|| false, |_, _| {
        panic!("must not fetch during backoff")
    });
    assert_eq!(retry["binance"], deadline);
    retry.insert("binance".into(), now() - 1);
    refresh_snapshot(&mut snapshot, &mut retry, &|| false, |_, _| {
        Ok(json!({"lastPrice":"100","priceChangePercent":"1"}))
    });
    assert_eq!(snapshot.quotes[0].price, Some(100.));
}
#[test]
fn historical_failure_keeps_current_price_but_not_wrong_percentage() {
    let mut c = Config::default();
    c.coins.truncate(1);
    c.change_period = "30d".into();
    let mut snapshot = Snapshot {
        quotes: vec![Quote::default()],
        config: c,
    };
    let mut retry = BTreeMap::new();
    refresh_snapshot(&mut snapshot, &mut retry, &|| false, |url, _| {
        if url.contains("klines") {
            anyhow::bail!("HTTP 503")
        }
        Ok(json!({"lastPrice":"110","priceChangePercent":"88"}))
    });
    let q = &snapshot.quotes[0];
    assert_eq!(q.price, Some(110.));
    assert!(q.change.is_none());
    assert!(q.change_error.is_some());
    assert!(q.error.is_none());
}
#[test]
fn cancellation_prevents_fetch() {
    let c = Config::default();
    let mut snapshot = Snapshot {
        quotes: vec![Quote::default(); c.coins.len()],
        config: c,
    };
    refresh_snapshot(&mut snapshot, &mut BTreeMap::new(), &|| true, |_, _| {
        panic!("cancelled")
    });
    assert!(snapshot.quotes.iter().all(|q| q.price.is_none()));
}
