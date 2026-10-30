#pragma once
#include "json.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ct {
using Json = nlohmann::json;
struct Source {
    std::string url, pricePath, changePath, openPath;
    std::map<std::string, std::string> headers;
    std::string historyUrl = "", historyPricePath = "", symbolStyle = "plain";
    std::map<std::string, std::string> symbolMap = {};
};
struct Coin {
    std::string label, symbol, source;
};
struct Config {
    int refreshSeconds = 30, rotationSeconds = 4, width = 150, offset = 16;
    std::string position = "left", theme = "system", currency = "$", changePeriod = "24h";
    std::string upColor = "#159F76", downColor = "#E05260";
    bool showChange = true;
    std::map<std::string, Source> sources;
    std::vector<Coin> coins;
};
struct Quote {
    std::optional<double> price, change;
    std::string error;
    std::string changeError;
    long long updated = 0;
    bool stale = false;
};

inline std::string DefaultConfig() {
    return R"({
  "refreshSeconds": 30,
  "rotationSeconds": 4,
  "width": 150,
  "position": "left",
  "offset": 16,
  "theme": "system",
  "currency": "$",
  "showChange": true,
  "changePeriod": "24h",
  "upColor": "#159F76",
  "downColor": "#E05260",
  "sources": {
    "coingecko": {
      "url": "https://api.coingecko.com/api/v3/simple/price?ids={ids}&vs_currencies=usd&include_24hr_change=true",
      "pricePath": "/{symbol}/usd",
      "changePath": "/{symbol}/usd_24h_change",
      "symbolMap": {"BTCUSDT":"bitcoin", "ETHUSDT":"ethereum", "SOLUSDT":"solana"}
    },
    "binance": {
      "url": "https://api.binance.com/api/v3/ticker/24hr?symbol={symbol}",
      "pricePath": "/lastPrice",
      "changePath": "/priceChangePercent",
      "historyUrl": "https://api.binance.com/api/v3/klines?symbol={symbol}&interval=1m&startTime={startTimeMs}&limit=1",
      "historyPricePath": "/0/1"
    },
    "okx": {
      "url": "https://www.okx.com/api/v5/market/ticker?instId={symbol}",
      "pricePath": "/data/0/last",
      "openPath": "/data/0/open24h",
      "symbolStyle": "dashed",
      "historyUrl": "https://www.okx.com/api/v5/market/history-candles?instId={symbol}&bar=1m&after={endTimeMs}&limit=1",
      "historyPricePath": "/data/0/1"
    }
  },
  "coins": [
    {"label": "BTC", "symbol": "BTCUSDT", "source": "binance"},
    {"label": "ETH", "symbol": "ETHUSDT", "source": "binance"},
    {"label": "SOL", "symbol": "SOLUSDT", "source": "binance"}
  ]
})";
}

inline void Require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
inline long long PeriodSeconds(const std::string &period) {
    if (period == "1h")
        return 3600;
    if (period == "4h")
        return 14400;
    if (period == "24h")
        return 86400;
    if (period == "7d")
        return 604800;
    if (period == "30d")
        return 2592000;
    throw std::runtime_error("请选择 1h、4h、24h、7d 或 30d。");
}
inline bool ValidColor(const std::string &text) {
    return text.size() == 7 && text[0] == '#' &&
           std::all_of(text.begin() + 1, text.end(),
                       [](unsigned char c) { return std::isxdigit(c) != 0; });
}
inline std::string CanonicalPair(std::string text) {
    if (text == "bitcoin")
        return "BTCUSDT";
    if (text == "ethereum")
        return "ETHUSDT";
    if (text == "solana")
        return "SOLUSDT";
    text.erase(
        std::remove_if(text.begin(), text.end(),
                       [](unsigned char c) { return c == '-' || c == '/' || std::isspace(c); }),
        text.end());
    for (auto &c : text)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}
inline std::pair<std::string, std::string> SplitPair(const std::string &text) {
    for (const auto *quote :
         {"USDT", "USDC", "BUSD", "FDUSD", "USD", "BTC", "ETH", "EUR", "BNB", "TRY"}) {
        const std::string suffix = quote;
        if (text.size() > suffix.size() &&
            text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0)
            return {text.substr(0, text.size() - suffix.size()), suffix};
    }
    return {text, ""};
}
inline Config ParseConfig(const std::string &text) {
    const auto j = Json::parse(text);
    Require(j.is_object(), "Config must be a JSON object.");
    Config c;
    c.refreshSeconds = j.value("refreshSeconds", 30);
    c.rotationSeconds = j.value("rotationSeconds", 4);
    c.width = j.value("width", 150);
    c.offset = j.value("offset", 16);
    c.position = j.value("position", "left");
    c.theme = j.value("theme", "system");
    c.currency = j.value("currency", "$");
    c.showChange = j.contains("changePeriod") ? j.value("showChange", true) : true;
    c.changePeriod = j.value("changePeriod", "24h");
    c.upColor = j.value("upColor", "#159F76");
    c.downColor = j.value("downColor", "#E05260");
    PeriodSeconds(c.changePeriod);
    Require(ValidColor(c.upColor) && ValidColor(c.downColor), "颜色需要使用 #RRGGBB 格式。");
    Require(c.refreshSeconds >= 5 && c.refreshSeconds <= 86400, "refreshSeconds must be 5..86400.");
    Require(c.rotationSeconds >= 1 && c.rotationSeconds <= 3600,
            "rotationSeconds must be 1..3600.");
    Require(c.width >= 80 && c.width <= 1200, "width must be 80..1200.");
    Require(c.offset >= 0 && c.offset <= 10000, "offset must be 0..10000.");
    Require(c.position == "left" || c.position == "right", "position must be left or right.");
    Require(c.theme == "system" || c.theme == "light" || c.theme == "dark",
            "theme must be system, light or dark.");
    Require(c.currency.size() <= 16, "currency is too long.");
    Require(j.contains("sources") && j["sources"].is_object() && !j["sources"].empty() &&
                j["sources"].size() <= 20,
            "sources must contain 1..20 data sources.");
    for (auto it = j["sources"].begin(); it != j["sources"].end(); ++it) {
        const auto &s = it.value();
        Source v;
        v.url = s.at("url").get<std::string>();
        v.pricePath = s.at("pricePath").get<std::string>();
        v.changePath = s.value("changePath", "");
        v.openPath = s.value("openPath", "");
        v.historyUrl = s.value("historyUrl", "");
        v.historyPricePath = s.value("historyPricePath", "");
        if (!s.contains("historyUrl") &&
            v.url.rfind("https://api.binance.com/api/v3/ticker/24hr", 0) == 0) {
            v.historyUrl = "https://api.binance.com/api/v3/"
                           "klines?symbol={symbol}&interval=1m&startTime={startTimeMs}&limit=1";
            v.historyPricePath = "/0/1";
        }
        if (!s.contains("historyUrl") &&
            v.url.rfind("https://www.okx.com/api/v5/market/ticker", 0) == 0) {
            v.historyUrl = "https://www.okx.com/api/v5/market/"
                           "history-candles?instId={symbol}&bar=1m&after={endTimeMs}&limit=1";
            v.historyPricePath = "/data/0/1";
        }
        v.symbolStyle = s.value("symbolStyle", it.key() == "okx" ? "dashed" : "plain");
        Require(v.symbolStyle == "plain" || v.symbolStyle == "dashed", "Invalid symbol format.");
        if (s.contains("symbolMap"))
            v.symbolMap = s["symbolMap"].get<std::map<std::string, std::string>>();
        else if (it.key() == "coingecko")
            v.symbolMap = {{"BTCUSDT", "bitcoin"}, {"ETHUSDT", "ethereum"}, {"SOLUSDT", "solana"}};
        Require(v.historyUrl.empty() || v.historyUrl.rfind("https://", 0) == 0 ||
                    v.historyUrl.rfind("http://", 0) == 0,
                "History URL must use http:// or https://.");
        Require(v.historyUrl.empty() == v.historyPricePath.empty(),
                "历史接口和历史价格路径需要一起填写。");
        Require(v.url.size() <= 8192 &&
                    (v.url.rfind("https://", 0) == 0 || v.url.rfind("http://", 0) == 0),
                "Source URL must use http:// or https://.");
        for (const auto *p : {&v.pricePath, &v.changePath, &v.openPath, &v.historyPricePath}) {
            Require(p->empty() || (*p)[0] == '/', "Paths use JSON Pointer, e.g. /data/0/last.");
            Json::json_pointer pointer(*p);
            (void)pointer;
        }
        if (s.contains("headers")) {
            v.headers = s["headers"].get<std::map<std::string, std::string>>();
            Require(v.headers.size() <= 20, "At most 20 headers per source.");
            for (const auto &[key, value] : v.headers) {
                Require(!key.empty() && key.find_first_of(":\r\n") == std::string::npos &&
                            value.find_first_of("\r\n") == std::string::npos,
                        "Invalid HTTP header.");
            }
        }
        c.sources.emplace(it.key(), std::move(v));
    }
    Require(j.contains("coins") && j["coins"].is_array() && !j["coins"].empty() &&
                j["coins"].size() <= 100,
            "coins must contain 1..100 entries.");
    for (const auto &v : j["coins"]) {
        Coin coin{v.at("label").get<std::string>(), v.at("symbol").get<std::string>(),
                  v.at("source").get<std::string>()};
        coin.symbol = CanonicalPair(coin.symbol);
        Require(coin.symbol.size() <= 40 && !SplitPair(coin.symbol).second.empty() &&
                    std::all_of(coin.symbol.begin(), coin.symbol.end(),
                                [](unsigned char c) { return std::isalnum(c) != 0 && c < 128; }),
                "交易对格式如 BTCUSDT、ETHUSDT。");
        Require(!coin.label.empty() && coin.label.size() <= 48 && !coin.symbol.empty() &&
                    coin.symbol.size() <= 200,
                "Coin label/symbol is empty or too long.");
        Require(c.sources.count(coin.source) != 0, "Unknown source: " + coin.source);
        c.coins.push_back(std::move(coin));
    }
    return c;
}
inline std::string SerializeConfig(const Config &c) {
    Json j{{"refreshSeconds", c.refreshSeconds},
           {"rotationSeconds", c.rotationSeconds},
           {"width", c.width},
           {"offset", c.offset},
           {"position", c.position},
           {"theme", c.theme},
           {"currency", c.currency},
           {"showChange", c.showChange}};
    j["changePeriod"] = c.changePeriod;
    j["upColor"] = c.upColor;
    j["downColor"] = c.downColor;
    j["sources"] = Json::object();
    for (const auto &[name, s] : c.sources) {
        auto &v = j["sources"][name];
        v = {{"url", s.url}, {"pricePath", s.pricePath}};
        if (!s.changePath.empty())
            v["changePath"] = s.changePath;
        if (!s.openPath.empty())
            v["openPath"] = s.openPath;
        if (!s.headers.empty())
            v["headers"] = s.headers;
        if (!s.historyUrl.empty()) {
            v["historyUrl"] = s.historyUrl;
            v["historyPricePath"] = s.historyPricePath;
        }
        if (s.symbolStyle != "plain")
            v["symbolStyle"] = s.symbolStyle;
        if (!s.symbolMap.empty())
            v["symbolMap"] = s.symbolMap;
    }
    j["coins"] = Json::array();
    for (const auto &coin : c.coins)
        j["coins"].push_back(
            {{"label", coin.label}, {"symbol", coin.symbol}, {"source", coin.source}});
    return j.dump(2);
}
inline std::string Replace(std::string text, const std::string &key, const std::string &value) {
    size_t pos = 0;
    while ((pos = text.find(key, pos)) != std::string::npos) {
        text.replace(pos, key.size(), value);
        pos += value.size();
    }
    return text;
}
inline std::string UrlEncode(const std::string &text) {
    std::string out;
    constexpr char hex[] = "0123456789ABCDEF";
    for (unsigned char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
            out += static_cast<char>(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}
inline std::string SourceSymbol(const Source &source, const Coin &coin) {
    if (const auto it = source.symbolMap.find(coin.symbol); it != source.symbolMap.end())
        return it->second;
    const auto pair = SplitPair(coin.symbol);
    return source.symbolStyle == "dashed" && !pair.second.empty() ? pair.first + "-" + pair.second
                                                                  : coin.symbol;
}
inline std::string RequestUrl(const Config &c, const Coin &coin, bool history = false,
                              long long nowMs = 0) {
    const auto &source = c.sources.at(coin.source);
    std::string ids;
    for (const auto &item : c.coins)
        if (item.source == coin.source) {
            if (!ids.empty())
                ids += ',';
            ids += UrlEncode(SourceSymbol(source, item));
        }
    auto url = Replace(Replace(history ? source.historyUrl : source.url, "{symbol}",
                               UrlEncode(SourceSymbol(source, coin))),
                       "{ids}", ids);
    url = Replace(url, "{period}", c.changePeriod);
    url = Replace(url, "{window}", c.changePeriod == "24h" ? "1d" : c.changePeriod);
    const long long start = (nowMs - PeriodSeconds(c.changePeriod) * 1000) / 60000 * 60000;
    url = Replace(url, "{startTimeMs}", std::to_string(start));
    return Replace(url, "{endTimeMs}", std::to_string(start + 59999));
}
inline std::string QuotePath(const std::string &path, const Coin &coin) {
    const auto symbol = Replace(Replace(coin.symbol, "~", "~0"), "/", "~1");
    return Replace(path, "{symbol}", symbol);
}
inline std::optional<double> NumberAt(const Json &j, const std::string &path) {
    if (path.empty())
        return {};
    try {
        const auto &v = j.at(Json::json_pointer(path));
        double n;
        if (v.is_number())
            n = v.get<double>();
        else if (v.is_string()) {
            const auto s = v.get<std::string>();
            if (s.empty())
                return {};
            size_t end = 0;
            n = std::stod(s, &end);
            if (end != s.size())
                return {};
        } else
            return {};
        if (std::isfinite(n))
            return n;
    } catch (const std::exception &) {
    }
    return {};
}
inline Quote ParseQuote(const Json &body, const Source &src, const Coin &coin,
                        const std::string &period = "24h") {
    Quote q;
    Coin mapped = coin;
    mapped.symbol = SourceSymbol(src, coin);
    q.price = NumberAt(body, QuotePath(src.pricePath, mapped));
    if (!q.price || *q.price <= 0)
        throw std::runtime_error("Price missing or invalid; check pricePath.");
    if (period != "24h" && src.url.find("{period}") == std::string::npos &&
        src.url.find("{window}") == std::string::npos) {
        if (src.historyUrl.empty())
            q.changeError = "此数据源不支持所选周期";
        return q;
    }
    q.change = NumberAt(body, QuotePath(src.changePath, mapped));
    if (!q.change)
        if (const auto open = NumberAt(body, QuotePath(src.openPath, mapped)); open && *open > 0)
            q.change = (*q.price / *open - 1) * 100;
    return q;
}
inline void ApplyHistoricalChange(Quote &q, const Json &history, const Source &source,
                                  const Coin &coin) {
    auto mapped = coin;
    mapped.symbol = SourceSymbol(source, coin);
    const auto reference = NumberAt(history, QuotePath(source.historyPricePath, mapped));
    if (!q.price || !reference || *reference <= 0)
        throw std::runtime_error("历史价格缺失");
    q.change = (*q.price / *reference - 1) * 100;
    q.changeError.clear();
}
inline std::string FormatPrice(double value) {
    std::ostringstream s;
    s.imbue(std::locale::classic());
    if (value < 0.000001) {
        s << std::setprecision(4) << value;
        return s.str();
    }
    const int decimals = value >= 1 ? 2 : value >= .01 ? 4 : 8;
    s << std::fixed << std::setprecision(decimals) << value;
    auto text = s.str();
    while (text.back() == '0')
        text.pop_back();
    if (text.back() == '.')
        text.pop_back();
    auto point = text.find('.');
    if (point == std::string::npos)
        point = text.size();
    for (int i = static_cast<int>(point) - 3; i > 0; i -= 3)
        text.insert(i, ",");
    return text;
}
inline std::string FormatChange(double value) {
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << (value >= 0 ? "+" : "") << std::fixed << std::setprecision(2) << value << "%";
    return s.str();
}
} // namespace ct
