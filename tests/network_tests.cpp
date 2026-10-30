#include "network.hpp"
#include <iostream>
int main() {
    auto c = ct::ParseConfig(ct::DefaultConfig());
    auto session = ct::OpenSession();
    if (!session)
        return 1;
    int failures = 0;
    for (const auto *source : {"binance", "okx", "coingecko"}) {
        ct::Coin coin{"BTC", "BTCUSDT", source};
        c.coins = {coin};
        for (const auto &period : std::vector<std::string>{"1h", "4h", "24h", "7d", "30d"}) {
            if (coin.source == "coingecko" && period != "24h")
                continue;
            c.changePeriod = period;
            try {
                const auto &src = c.sources.at(coin.source);
                auto q = ct::ParseQuote(ct::FetchJson(session.get(), ct::RequestUrl(c, coin), src),
                                        src, coin, period);
                if (period != "24h") {
                    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count();
                    const auto history =
                        ct::FetchJson(session.get(), ct::RequestUrl(c, coin, true, now), src);
                    ct::ApplyHistoricalChange(q, history, src, coin);
                    auto timestamp =
                        ct::NumberAt(history, coin.source == "binance" ? "/0/0" : "/data/0/0");
                    const auto expected = (now - ct::PeriodSeconds(period) * 1000) / 60000 * 60000;
                    if (!timestamp || std::abs(*timestamp - static_cast<double>(expected)) > 120000)
                        throw std::runtime_error("Historical candle is outside requested period.");
                }
                std::cout << source << ' ' << period << " price=" << *q.price
                          << " change=" << (q.change ? ct::FormatChange(*q.change) : "missing")
                          << '\n';
                if (!q.change || *q.price <= 0)
                    ++failures;
            } catch (const std::exception &e) {
                std::cout << source << ' ' << period << ": " << e.what() << '\n';
                ++failures;
            }
        }
    }
    return failures ? 1 : 0;
}