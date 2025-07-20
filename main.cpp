// #include "logger.hpp"
#include "consumer.hpp"
#include <chrono>
// #include <cstdint>
#include <mach/mach_time.h>
// inline uint64_t rdtsc()
// {
//     unsigned int hi, lo;
//     asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
//     return ((uint64_t)hi << 32) | lo;
// }
mach_timebase_info_data_t info;

inline uint64_t latency(uint64_t cycles)
{
    // uint64_t nanoseconds = (uint64_t)cycles * 1e9 / 3e9;
    return (uint64_t)cycles * info.numer / info.denom;
}
void trader_thread_function(uint32_t log_id)
{
    for (int i = 0; i < 10000; ++i)
    {
        uint64_t start = mach_absolute_time();
        log(log_id, "Order %d price %.2f by Users %s, %s, %s, %s Order %d, %d", i, 123.45 + i, "Akshdeep", "Arshdeep", "Vaibhav", "Piyush", i, i * 2);
        uint64_t end = mach_absolute_time();
        // ... other logic ...
        std::cout << "Latency: " << latency(end - start) << std::endl;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}

int main()
{
    mach_timebase_info(&info);
    LogConsumer consumer;
    std::thread consumer_thread([&consumer]()
                                { consumer.run(); });

    // std::vector<std::thread> producers;
    // for (int i = 0; i < 4; ++i)
    //     producers.emplace_back(trader_thread_function);

    // for (auto &t : producers)
    //     t.join();
    trader_thread_function(32);
    // Signal consumer to exit as needed; omitted for brevity
    consumer.stop();
    consumer_thread.join();
}
