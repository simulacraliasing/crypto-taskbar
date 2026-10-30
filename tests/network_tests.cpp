#include "network.hpp"
#include <iostream>
int main() {
    auto c = ct::ParseConfig(ct::DefaultConfig());
    auto session = ct::OpenSession();
    if (!session)
        return 1;
    int failures = 0;
    for (const auto &coin : std::vector<ct::Coin>{{"BTC", "bitcoin", "coingecko"},
                                                  {"BTC", "BTCUSDT", "binance"},
                                                  {"BTC", "BTC-USDT", "okx"}}) {
        try {
            auto q = ct::ParseQuote(
                ct::FetchJson(session.get(), ct::RequestUrl(c, coin), c.sources.at(coin.source)),
                c.sources.at(coin.source), coin);
            std::cout << coin.source << ": price=" << *q.price
                      << " change=" << (q.change ? std::to_string(*q.change) : "missing") << '\n';
            if (!q.change || *q.price <= 0)
                ++failures;
        } catch (const std::exception &e) {
            std::cout << coin.source << ": " << e.what() << '\n';
            ++failures;
        }
    }
    return failures ? 1 : 0;
}
