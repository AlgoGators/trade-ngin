#include <iostream>
#include <vector>
#include <algorithm>
#include <random>
#include <chrono>
#include <functional>
#include <iomanip>
#include <string>
#include <thread>
#include <atomic>

template <typename T, typename Compare>
void merge_sort(typename std::vector<T>::iterator begin, typename std::vector<T>::iterator end, Compare comp)
{
    if (end - begin <= 1)
        return;

    auto middle = begin + (end - begin) / 2;
    merge_sort<T, Compare>(begin, middle, comp);
    merge_sort<T, Compare>(middle, end, comp);

    std::vector<T> buffer(end - begin);
    std::merge(begin, middle, middle, end, buffer.begin(), comp);
    std::copy(buffer.begin(), buffer.end(), begin);
}

template <typename T, typename Compare>
void merge_sort_thread_v1(typename std::vector<T>::iterator begin, typename std::vector<T>::iterator end, Compare comp)
{
    if (end - begin <= 1)
        return;

    auto middle = begin + (end - begin) / 2;

    if (end - begin > 1000)
    {
        std::thread leftThread(merge_sort<T, Compare>, begin, middle, comp);
        std::thread rightThread(merge_sort<T, Compare>, middle, end, comp);

        leftThread.join();
        rightThread.join();
    }
    else
    {
        merge_sort<T, Compare>(begin, middle, comp);
        merge_sort<T, Compare>(middle, end, comp);
    }

    std::vector<T> buffer(end - begin);
    std::merge(begin, middle, middle, end, buffer.begin(), comp);
    std::copy(buffer.begin(), buffer.end(), begin);
}

template <typename T, typename Compare>
void merge_sort_thread_race(typename std::vector<T>::iterator begin, typename std::vector<T>::iterator end, Compare comp)
{
    if (end - begin <= 1)
        return;

    auto middle = begin + (end - begin) / 2;

    if (end - begin > 1000)
    {

        std::thread leftThread(merge_sort_thread_v1<T, Compare>, begin, middle, comp);
        std::thread rightThread(merge_sort_thread_v1<T, Compare>, middle, end, comp);

        leftThread.join();
        rightThread.join();
    }
    else
    {
        merge_sort_thread_v1<T, Compare>(begin, middle, comp);
        merge_sort_thread_v1<T, Compare>(middle, end, comp);
    }

    std::vector<T> buffer(end - begin);

    std::thread mergeThread([&]()
                            { std::merge(begin, middle, middle, end, buffer.begin(), comp); });

    mergeThread.join();

    std::copy(buffer.begin(), buffer.end(), begin);
}

struct StockPrice
{
    std::string symbol;
    double price;
    long timestamp;

    static StockPrice random(std::mt19937 &gen)
    {
        static std::vector<std::string> symbols = {"AAPL", "MSFT", "GOOGL", "AMZN", "FB", "TSLA", "JPM", "V", "JNJ", "WMT"};
        static std::uniform_int_distribution<> symbol_dist(0, symbols.size() - 1);
        static std::uniform_real_distribution<> price_dist(50.0, 1000.0);
        static std::uniform_int_distribution<long> time_dist(1600000000, 1630000000);

        return {
            symbols[symbol_dist(gen)],
            price_dist(gen),
            time_dist(gen)};
    }
};

std::vector<StockPrice> generate_dataset(size_t size)
{
    std::random_device rd;
    std::mt19937 gen(rd());

    std::vector<StockPrice> data(size);
    for (auto &item : data)
    {
        item = StockPrice::random(gen);
    }

    return data;
}

template <typename SortFunc>
double benchmark_sort(std::vector<StockPrice> data, SortFunc sort_func, const std::string &algorithm_name, std::vector<double> &leaked_time)
{
    auto start = std::chrono::high_resolution_clock::now();

    sort_func(data);

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end - start;
    leaked_time.push_back(duration.count());
    std::cout << std::left << std::setw(15) << algorithm_name
              << std::setw(12) << data.size()
              << std::fixed << std::setprecision(2) << duration.count() << " ms"
              << std::endl;

    return duration.count();
}

void unsafe_increment(int &value)
{
    for (int i = 0; i < 1000; ++i)
    {
        value++; // Race condition here!!!!!!!!!
    }
}
int main()
{

    std::cout << "=============================================" << std::endl;

    std::vector<double> *leak_time = new std::vector<double>();
    // std::unique_ptr<std::vector<double>> leak_time = std::make_unique<std::vector<double>>();   You can switch 161 or 162

    std::cout << "Sorting Algorithm Benchmark for Financial Data" << std::endl;
    std::cout << "=============================================" << std::endl;

    std::vector<size_t> dataset_sizes = {1000, 10000, 100000, 1000000};

    std::cout << std::left << std::setw(15) << "Algorithm"
              << std::setw(12) << "Data Size"
              << "Time" << std::endl;
    std::cout << "---------------------------------------------" << std::endl;

    std::cout << "\nRandom Data:" << std::endl;
    for (auto size : dataset_sizes)
    {
        auto data = generate_dataset(size);

        // std::sort
        auto std_sort_data = data;
        benchmark_sort(std_sort_data, [](std::vector<StockPrice> &d)
                       { std::sort(d.begin(), d.end(), [](const StockPrice &a, const StockPrice &b)
                                   { return a.price < b.price; }); }, "std::sort", *leak_time);

        // Custom MergeSort
        if (size <= 1000000)
        {
            auto merge_sort_data = data;
            benchmark_sort(merge_sort_data, [](std::vector<StockPrice> &d)
                           { merge_sort<StockPrice>(d.begin(), d.end(), [](const StockPrice &a, const StockPrice &b)
                                                    { return a.price < b.price; }); }, "MergeSort", *leak_time);
        }

        // Custom MergeSort with Thread
        if (size <= 1000000)
        {
            auto merge_sort_data = data;
            benchmark_sort(merge_sort_data, [](std::vector<StockPrice> &d)
                           { merge_sort_thread_v1<StockPrice>(d.begin(), d.end(), [](const StockPrice &a, const StockPrice &b)
                                                              { return a.price < b.price; }); }, "MergeSort w/ Threads", *leak_time);
        }

        // Custom MergeSort with Race condition (jk it worked </3)
        if (size <= 1000000)
        {
            auto merge_sort_data = data;
            benchmark_sort(merge_sort_data, [](std::vector<StockPrice> &d)
                           { merge_sort_thread_race<StockPrice>(d.begin(), d.end(), [](const StockPrice &a, const StockPrice &b)
                                                                { return a.price < b.price; }); }, "MergeSort w/ Threads v2 ", *leak_time);
        }

        std::cout << "---------------------------------------------" << std::endl;
    }

    return 0;
}
