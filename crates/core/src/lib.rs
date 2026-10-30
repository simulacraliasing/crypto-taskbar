use anyhow::{Context, Result, bail, ensure};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::{
    collections::BTreeMap,
    fs,
    io::Write,
    path::{Path, PathBuf},
};

pub const VERSION: &str = env!("CARGO_PKG_VERSION");
pub const PERIODS: [&str; 5] = ["1h", "4h", "24h", "7d", "30d"];
pub const MAX_CONFIG_BYTES: u64 = 256 * 1024;

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
#[serde(default, rename_all = "camelCase")]
pub struct Source {
    pub url: String,
    pub price_path: String,
    pub change_path: String,
    pub open_path: String,
    pub history_url: String,
    pub history_price_path: String,
    pub symbol_style: String,
    pub symbol_map: BTreeMap<String, String>,
    pub headers: BTreeMap<String, String>,
}
impl Default for Source {
    fn default() -> Self {
        Self {
            url: String::new(),
            price_path: String::new(),
            change_path: String::new(),
            open_path: String::new(),
            history_url: String::new(),
            history_price_path: String::new(),
            symbol_style: "plain".into(),
            symbol_map: BTreeMap::new(),
            headers: BTreeMap::new(),
        }
    }
}
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct Coin {
    pub label: String,
    pub symbol: String,
    pub source: String,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
#[serde(default, rename_all = "camelCase")]
pub struct Config {
    pub refresh_seconds: u64,
    pub rotation_seconds: u64,
    pub width: i32,
    pub offset: i32,
    pub position: String,
    pub theme: String,
    pub currency: String,
    pub change_period: String,
    pub up_color: String,
    pub down_color: String,
    pub show_change: bool,
    pub sources: BTreeMap<String, Source>,
    pub coins: Vec<Coin>,
}
impl Default for Config {
    fn default() -> Self {
        let example: Value = serde_json::from_str(include_str!("../../../config.example.json"))
            .expect("bundled example");
        Self {
            refresh_seconds: 30,
            rotation_seconds: 4,
            width: 150,
            offset: 16,
            position: "left".into(),
            theme: "system".into(),
            currency: "$".into(),
            change_period: "24h".into(),
            up_color: "#159F76".into(),
            down_color: "#E05260".into(),
            show_change: true,
            sources: serde_json::from_value(example["sources"].clone()).expect("bundled sources"),
            coins: ["BTC", "ETH", "SOL"]
                .into_iter()
                .map(|name| Coin {
                    label: name.into(),
                    symbol: format!("{name}USDT"),
                    source: "binance".into(),
                })
                .collect(),
        }
    }
}
impl Config {
    pub fn parse(bytes: &[u8]) -> Result<Self> {
        ensure!(
            bytes.len() as u64 <= MAX_CONFIG_BYTES,
            "配置文件超过 256 KiB。"
        );
        let bytes = bytes.strip_prefix(&[0xef, 0xbb, 0xbf]).unwrap_or(bytes);
        let value: Value = serde_json::from_slice(bytes).context("配置文件不是有效的 JSON")?;
        ensure!(
            value["sources"].is_object() && value["coins"].is_array(),
            "配置需要数据源和币种列表。"
        );
        let mut config: Self = serde_json::from_value(value.clone())?;
        if value.get("changePeriod").is_none() {
            config.show_change = true;
        }
        let defaults = Self::default();
        for (name, source) in &mut config.sources {
            let raw = &value["sources"][name];
            if name == "okx" && raw.get("symbolStyle").is_none() {
                source.symbol_style = "dashed".into();
            }
            if name == "coingecko" && raw.get("symbolMap").is_none() {
                source.symbol_map = defaults.sources["coingecko"].symbol_map.clone();
            }
            if raw.get("historyUrl").is_none() {
                for preset in ["binance", "okx"] {
                    let original = &defaults.sources[preset];
                    if source
                        .url
                        .starts_with(original.url.split('?').next().unwrap())
                    {
                        source.history_url = original.history_url.clone();
                        source.history_price_path = original.history_price_path.clone();
                    }
                }
            }
        }
        for coin in &mut config.coins {
            coin.symbol = canonical_pair(&coin.symbol);
        }
        config.validate()?;
        Ok(config)
    }
    pub fn validate(&self) -> Result<()> {
        ensure!(
            (5..=86400).contains(&self.refresh_seconds),
            "行情刷新间隔应为 5–86400 秒。"
        );
        ensure!(
            (1..=3600).contains(&self.rotation_seconds),
            "轮播间隔应为 1–3600 秒。"
        );
        ensure!((80..=1200).contains(&self.width), "显示宽度应为 80–1200。");
        ensure!((0..=10000).contains(&self.offset), "边缘距离应为 0–10000。");
        ensure!(
            ["left", "right"].contains(&self.position.as_str()),
            "显示位置无效。"
        );
        ensure!(
            ["system", "light", "dark"].contains(&self.theme.as_str()),
            "主题无效。"
        );
        period_seconds(&self.change_period)?;
        parse_color(&self.up_color)?;
        parse_color(&self.down_color)?;
        ensure!(self.currency.len() <= 16, "价格前缀过长。");
        ensure!(
            (1..=20).contains(&self.sources.len()),
            "请配置 1–20 个数据源。"
        );
        ensure!(
            (1..=100).contains(&self.coins.len()),
            "请配置 1–100 个币种。"
        );
        for (name, source) in &self.sources {
            ensure!(
                !name.trim().is_empty() && name.len() <= 100,
                "数据源名称为空或过长。"
            );
            source
                .validate()
                .with_context(|| format!("数据源 {name}"))?;
        }
        for coin in &self.coins {
            ensure!(
                !coin.label.trim().is_empty() && coin.label.len() <= 48,
                "币种名称为空或过长。"
            );
            ensure!(
                coin.symbol.len() <= 40
                    && coin.symbol.bytes().all(|c| c.is_ascii_alphanumeric())
                    && split_pair(&coin.symbol).is_some(),
                "交易对格式如 BTCUSDT、ETHUSDT。"
            );
            ensure!(
                self.sources.contains_key(&coin.source),
                "{} 的数据源不存在。",
                coin.label
            );
        }
        Ok(())
    }
    pub fn load(path: &Path) -> Result<Self> {
        ensure!(
            fs::metadata(path)?.len() <= MAX_CONFIG_BYTES,
            "配置文件超过 256 KiB。"
        );
        Self::parse(&fs::read(path)?)
    }
    pub fn save(&self, path: &Path) -> Result<()> {
        self.validate()?;
        let parent = path.parent().context("配置路径无效")?;
        fs::create_dir_all(parent)?;
        let bytes = serde_json::to_vec_pretty(self)?;
        ensure!(
            bytes.len() as u64 <= MAX_CONFIG_BYTES,
            "配置文件超过 256 KiB。"
        );
        let temp = path.with_extension("json.tmp");
        let mut file = fs::File::create(&temp)?;
        file.write_all(&bytes)?;
        file.sync_all()?;
        drop(file);
        if path.exists() {
            fs::copy(path, path.with_extension("json.bak"))?;
        }
        fs::rename(&temp, path).context("无法替换配置文件")?;
        Ok(())
    }
    pub fn request_url(&self, coin: &Coin, history: bool, now_ms: i64) -> Result<String> {
        let source = self.sources.get(&coin.source).context("数据源不存在")?;
        let symbol = source.symbol(coin);
        let ids = self
            .coins
            .iter()
            .filter(|c| c.source == coin.source)
            .map(|c| encode(&source.symbol(c)))
            .collect::<Vec<_>>()
            .join(",");
        let start =
            (now_ms - period_seconds(&self.change_period)? * 1000).div_euclid(60000) * 60000;
        let url = if history {
            &source.history_url
        } else {
            &source.url
        };
        Ok(url
            .replace("{symbol}", &encode(&symbol))
            .replace("{ids}", &ids)
            .replace("{period}", &self.change_period)
            .replace(
                "{window}",
                if self.change_period == "24h" {
                    "1d"
                } else {
                    &self.change_period
                },
            )
            .replace("{startTimeMs}", &start.to_string())
            .replace("{endTimeMs}", &(start + 59999).to_string()))
    }
}
impl Source {
    pub fn validate(&self) -> Result<()> {
        validate_url(&self.url)?;
        if !self.history_url.is_empty() {
            validate_url(&self.history_url)?;
        }
        ensure!(
            self.history_url.is_empty() == self.history_price_path.is_empty(),
            "历史接口和历史价格路径需要一起填写。"
        );
        ensure!(
            ["plain", "dashed"].contains(&self.symbol_style.as_str()),
            "交易对格式无效。"
        );
        for path in [
            &self.price_path,
            &self.change_path,
            &self.open_path,
            &self.history_price_path,
        ] {
            ensure!(
                valid_pointer(path),
                "路径使用 JSON Pointer，例如 /data/0/last。"
            );
        }
        ensure!(
            self.headers.len() <= 20 && self.symbol_map.len() <= 1000,
            "请求头或代码映射过多。"
        );
        for (name, value) in &self.headers {
            ensure!(
                !name.is_empty()
                    && name
                        .bytes()
                        .all(|b| b.is_ascii_alphanumeric() || b"!#$%&'*+-.^_`|~".contains(&b))
                    && !value.contains(['\r', '\n']),
                "请求头格式无效。"
            );
        }
        Ok(())
    }
    pub fn symbol(&self, coin: &Coin) -> String {
        if let Some(mapped) = self.symbol_map.get(&coin.symbol) {
            return mapped.clone();
        }
        if self.symbol_style == "dashed"
            && let Some((base, quote)) = split_pair(&coin.symbol)
        {
            return format!("{base}-{quote}");
        }
        coin.symbol.clone()
    }
    pub fn path(&self, template: &str, coin: &Coin) -> String {
        template.replace(
            "{symbol}",
            &self.symbol(coin).replace('~', "~0").replace('/', "~1"),
        )
    }
}
pub fn default_config_path() -> Result<PathBuf> {
    Ok(
        PathBuf::from(std::env::var_os("LOCALAPPDATA").context("LOCALAPPDATA 不可用")?)
            .join("CryptoTaskbar")
            .join("config.json"),
    )
}
pub fn period_seconds(period: &str) -> Result<i64> {
    Ok(match period {
        "1h" => 3600,
        "4h" => 14400,
        "24h" => 86400,
        "7d" => 604800,
        "30d" => 2592000,
        _ => bail!("请选择 1h、4h、24h、7d 或 30d。"),
    })
}
pub fn parse_color(text: &str) -> Result<u32> {
    ensure!(
        text.len() == 7
            && text.starts_with('#')
            && text[1..].bytes().all(|c| c.is_ascii_hexdigit()),
        "颜色需要使用 #RRGGBB 格式。"
    );
    Ok(u32::from_str_radix(&text[1..], 16)?)
}
pub fn canonical_pair(text: &str) -> String {
    match text {
        "bitcoin" => "BTCUSDT".into(),
        "ethereum" => "ETHUSDT".into(),
        "solana" => "SOLUSDT".into(),
        _ => text
            .chars()
            .filter(|c| !c.is_whitespace() && *c != '/' && *c != '-')
            .collect::<String>()
            .to_ascii_uppercase(),
    }
}
pub fn split_pair(text: &str) -> Option<(&str, &str)> {
    for quote in [
        "FDUSD", "USDT", "USDC", "BUSD", "USD", "BTC", "ETH", "EUR", "BNB", "TRY",
    ] {
        if let Some(base) = text.strip_suffix(quote).filter(|s| !s.is_empty()) {
            return Some((base, &text[base.len()..]));
        }
    }
    None
}
pub fn encode(text: &str) -> String {
    let mut out = String::new();
    for byte in text.bytes() {
        if byte.is_ascii_alphanumeric() || b"-_.~".contains(&byte) {
            out.push(byte as char);
        } else {
            use std::fmt::Write;
            write!(&mut out, "%{byte:02X}").unwrap();
        }
    }
    out
}
pub fn validate_url(text: &str) -> Result<()> {
    ensure!(text.len() <= 8192, "接口 URL 过长。");
    let parsed = url::Url::parse(text).context("接口 URL 无效")?;
    ensure!(
        ["http", "https"].contains(&parsed.scheme()) && parsed.host_str().is_some(),
        "接口 URL 需要 http:// 或 https://。"
    );
    ensure!(
        parsed.username().is_empty() && parsed.password().is_none(),
        "认证信息请通过请求头传入。"
    );
    Ok(())
}
fn valid_pointer(path: &str) -> bool {
    if !path.is_empty() && !path.starts_with('/') {
        return false;
    }
    let mut chars = path.chars();
    while let Some(c) = chars.next() {
        if c == '~' && !matches!(chars.next(), Some('0' | '1')) {
            return false;
        }
    }
    true
}
pub fn number_at(body: &Value, pointer: &str) -> Option<f64> {
    let value = body.pointer(pointer)?;
    let number = if let Some(text) = value.as_str() {
        if text.trim() != text {
            return None;
        }
        text.parse::<f64>().ok()?
    } else {
        value.as_f64()?
    };
    number.is_finite().then_some(number)
}
#[derive(Clone, Debug, Default, Serialize)]
pub struct Quote {
    pub price: Option<f64>,
    pub change: Option<f64>,
    pub error: Option<String>,
    pub change_error: Option<String>,
    pub updated: Option<u64>,
    pub stale: bool,
}
impl Quote {
    pub fn parse(body: &Value, source: &Source, coin: &Coin, period: &str) -> Result<Self> {
        let price = number_at(body, &source.path(&source.price_path, coin))
            .filter(|v| *v > 0.)
            .context("价格缺失，请检查价格路径。")?;
        let mut quote = Self {
            price: Some(price),
            ..Self::default()
        };
        if period != "24h" && !source.url.contains("{period}") && !source.url.contains("{window}") {
            if source.history_url.is_empty() {
                quote.change_error = Some("此数据源不支持所选周期".into());
            }
            return Ok(quote);
        }
        if !source.change_path.is_empty() {
            quote.change = number_at(body, &source.path(&source.change_path, coin));
        }
        if quote.change.is_none()
            && !source.open_path.is_empty()
            && let Some(reference) =
                number_at(body, &source.path(&source.open_path, coin)).filter(|v| *v > 0.)
        {
            quote.change = Some(change_from(price, reference)?);
        }
        Ok(quote)
    }
    pub fn apply_history(&mut self, body: &Value, source: &Source, coin: &Coin) -> Result<()> {
        let reference = number_at(body, &source.path(&source.history_price_path, coin))
            .filter(|v| *v > 0.)
            .context("历史价格缺失")?;
        self.change = Some(change_from(self.price.context("当前价格缺失")?, reference)?);
        self.change_error = None;
        Ok(())
    }
    pub fn failed(&mut self, error: String) {
        self.error = Some(error);
        self.stale = self.price.is_some();
    }
}
pub fn change_from(price: f64, reference: f64) -> Result<f64> {
    let change = (price / reference - 1.) * 100.;
    ensure!(change.is_finite(), "涨跌幅超出有效范围。");
    Ok(change)
}
pub fn format_change(value: f64) -> String {
    format!("{:+.2}%", if value.abs() < 0.005 { 0. } else { value })
}
pub fn format_price(value: f64) -> String {
    if value > 0. && value < 0.000001 {
        let text = format!("{value:.3e}");
        let (mantissa, exponent) = text.split_once('e').unwrap();
        return format!(
            "{}e{}",
            mantissa.trim_end_matches('0').trim_end_matches('.'),
            exponent
        );
    }
    let decimals = if value >= 1. {
        2
    } else if value >= 0.01 {
        4
    } else {
        8
    };
    let raw = format!("{value:.decimals$}");
    let raw = raw.trim_end_matches('0').trim_end_matches('.');
    let (integer, fraction) = raw
        .split_once('.')
        .map_or((raw, None), |(a, b)| (a, Some(b)));
    let mut grouped = String::new();
    for (i, c) in integer.chars().enumerate() {
        if i > 0 && (integer.len() - i) % 3 == 0 {
            grouped.push(',');
        }
        grouped.push(c);
    }
    if let Some(fraction) = fraction {
        grouped.push('.');
        grouped.push_str(fraction);
    }
    grouped
}
pub fn expand_header(text: &str) -> Result<String> {
    expand_header_with(text, |name| std::env::var(name).ok())
}
pub fn expand_header_with(
    text: &str,
    mut lookup: impl FnMut(&str) -> Option<String>,
) -> Result<String> {
    let mut result = String::new();
    let mut rest = text;
    while let Some(start) = rest.find('%') {
        result.push_str(&rest[..start]);
        rest = &rest[start + 1..];
        if let Some(end) = rest.find('%') {
            let name = &rest[..end];
            ensure!(!name.is_empty(), "环境变量名称为空。");
            result.push_str(&lookup(name).with_context(|| format!("环境变量 {name} 未设置"))?);
            rest = &rest[end + 1..];
        } else {
            result.push('%');
            break;
        }
    }
    result.push_str(rest);
    ensure!(!result.contains(['\r', '\n']), "展开后的请求头包含换行。");
    Ok(result)
}
