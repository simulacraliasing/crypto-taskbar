#pragma once
#include "json.hpp"
#include <algorithm>
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
};
struct Coin {
    std::string label, symbol, source;
};
struct Config {
    int refreshSeconds = 30, rotationSeconds = 4, width = 150, offset = 16;
    std::string position = "left", theme = "system", currency = "$";
    bool showChange = false;
    std::map<std::string, Source> sources;
    std::vector<Coin> coins;
};
struct Quote {
    std::optional<double> price, change;
    std::string error;
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
  "showChange": false,
  "sources": {
    "coingecko": {
      "url": "https://api.coingecko.com/api/v3/simple/price?ids={ids}&vs_currencies=usd&include_24hr_change=true",
      "pricePath": "/{symbol}/usd",
      "changePath": "/{symbol}/usd_24h_change"
    },
    "binance": {
      "url": "https://api.binance.com/api/v3/ticker/24hr?symbol={symbol}",
      "pricePath": "/lastPrice",
      "changePath": "/priceChangePercent"
    },
    "okx": {
      "url": "https://www.okx.com/api/v5/market/ticker?instId={symbol}",
      "pricePath": "/data/0/last",
      "openPath": "/data/0/open24h"
    }
  },
  "coins": [
    {"label": "BTC", "symbol": "bitcoin", "source": "coingecko"},
    {"label": "ETH", "symbol": "ethereum", "source": "coingecko"},
    {"label": "SOL", "symbol": "solana", "source": "coingecko"}
  ]
})";
}

inline void Require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
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
    c.showChange = j.value("showChange", false);
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
        Require(v.url.size() <= 8192 &&
                    (v.url.rfind("https://", 0) == 0 || v.url.rfind("http://", 0) == 0),
                "Source URL must use http:// or https://.");
        for (const auto *p : {&v.pricePath, &v.changePath, &v.openPath}) {
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
inline std::string RequestUrl(const Config &c, const Coin &coin) {
    std::string ids;
    for (const auto &item : c.coins)
        if (item.source == coin.source) {
            if (!ids.empty())
                ids += ',';
            ids += UrlEncode(item.symbol);
        }
    return Replace(Replace(c.sources.at(coin.source).url, "{symbol}", UrlEncode(coin.symbol)),
                   "{ids}", ids);
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
inline Quote ParseQuote(const Json &body, const Source &src, const Coin &coin) {
    Quote q;
    q.price = NumberAt(body, QuotePath(src.pricePath, coin));
    if (!q.price || *q.price <= 0)
        throw std::runtime_error("Price missing or invalid; check pricePath.");
    q.change = NumberAt(body, QuotePath(src.changePath, coin));
    if (!q.change)
        if (const auto open = NumberAt(body, QuotePath(src.openPath, coin)); open && *open > 0)
            q.change = (*q.price / *open - 1) * 100;
    return q;
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
