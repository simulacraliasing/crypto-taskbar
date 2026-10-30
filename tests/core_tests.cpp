#include "core.hpp"
#include <iostream>
int main() {
    int checks = 0;
    auto expect = [&](bool ok) {
        ++checks;
        if (!ok)
            throw std::runtime_error("Failed check " + std::to_string(checks));
    };
    auto rejects = [&](const ct::Json &config) {
        try {
            ct::ParseConfig(config.dump());
            return false;
        } catch (...) {
            return true;
        }
    };
    auto c = ct::ParseConfig(ct::DefaultConfig());
    expect(c.coins.size() == 3);
    expect(ct::RequestUrl(c, c.coins[0]) == ct::RequestUrl(c, c.coins[1]));
    expect(ct::RequestUrl(c, c.coins[0]).find("bitcoin,ethereum,solana") != std::string::npos);
    auto j = ct::Json::parse(R"({"bitcoin":{"usd":1234.56,"usd_24h_change":0}})");
    auto q = ct::ParseQuote(j, c.sources.at("coingecko"), c.coins[0]);
    expect(q.price == 1234.56 && q.change == 0.0);
    auto src = c.sources.at("okx");
    auto okx = ct::Json::parse(R"({"data":[{"last":"110","open24h":"100"}]})");
    expect(std::abs(*ct::ParseQuote(okx, src, c.coins[0]).change - 10.0) < 1e-9);
    for (const auto &raw :
         {R"({"n":null})", R"({"n":"NaN"})", R"({"n":"1oops"})", R"({"n":true})", R"({"n":""})"})
        expect(!ct::NumberAt(ct::Json::parse(raw), "/n"));
    expect(ct::NumberAt(ct::Json::parse(R"({"n":"0.000001"})"), "/n") == 0.000001);
    expect(!ct::NumberAt(j, "/missing/0"));
    ct::Coin escaped{"Test", "a/b~c", "coingecko"};
    expect(ct::NumberAt(ct::Json::parse(R"({"a/b~c":{"usd":42}})"),
                        ct::QuotePath("/{symbol}/usd", escaped)) == 42);
    expect(ct::UrlEncode("BTC/USDT &") == "BTC%2FUSDT%20%26");
    expect(ct::FormatPrice(123456.78) == "123,456.78");
    expect(ct::FormatPrice(0.00001234) == "0.00001234");
    expect(ct::FormatPrice(0.000000001).find("e-") != std::string::npos);
    expect(ct::FormatChange(0) == "+0.00%");
    for (const auto &patch : {ct::Json{{"refreshSeconds", 0}}, ct::Json{{"width", 20}},
                              ct::Json{{"coins", ct::Json::array()}}, ct::Json{{"theme", "bad"}}}) {
        auto x = ct::Json::parse(ct::DefaultConfig());
        x.update(patch);
        expect(rejects(x));
    }
    auto bad = ct::Json::parse(ct::DefaultConfig());
    bad["coins"][0]["source"] = "missing";
    expect(rejects(bad));
    bad = ct::Json::parse(ct::DefaultConfig());
    bad["sources"]["coingecko"]["pricePath"] = "/~bad";
    expect(rejects(bad));
    bad = ct::Json::parse(ct::DefaultConfig());
    bad["sources"]["coingecko"]["headers"] = {{"Authorization", "x\r\nInjected: yes"}};
    expect(rejects(bad));
    bad = ct::Json::parse(ct::DefaultConfig());
    while (bad["coins"].size() < 100)
        bad["coins"].push_back(bad["coins"][0]);
    expect(!rejects(bad));
    bad["coins"].push_back(bad["coins"][0]);
    expect(rejects(bad));
    bool threw = false;
    try {
        ct::ParseQuote(ct::Json{{"price", -1}}, ct::Source{"", "/price", "", "", {}}, c.coins[0]);
    } catch (...) {
        threw = true;
    }
    expect(threw);
    std::cout << checks << " checks passed\n";
}
