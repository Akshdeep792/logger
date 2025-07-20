// consumer.hpp
#pragma once

#include "logger.hpp"
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <iostream>
#include <thread>
#include <chrono>
#include <mutex>

class LogConsumer
{
public:
    void run()
    {
        while (!_stop_flag)
        {
            // Poll every thread buffer for logs
            Logger::instance().for_each_buffer([this](ThreadRingBuffer *buf)
                                               {
                LogMsg msg;
                while (buf->try_consume(msg)) {
                    write_log(msg);
                } });

            // To avoid busy loop: tune sleep/yield as needed.
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
        flush_all_files();
    }

    void stop()
    {
        _stop_flag = true;
    }

private:
    void write_log(const LogMsg &msg)
    {
        std::ofstream &ofs = get_file_stream(msg.log_id);
        ofs << "[" << msg.timestamp << "][T" << msg.thread_id << "] ";
        for (size_t i = 0; i < msg.arg_count; ++i)
        {
            const LogArg &a = msg.args[i];
            switch (a.type)
            {
            case LogArgType::I64:
                ofs << a.i64 << " ";
                break;
            case LogArgType::U64:
                ofs << a.u64 << " ";
                break;
            case LogArgType::F64:
                ofs << std::setprecision(15) << a.f64 << " ";
                break;
            case LogArgType::CString:
                ofs.write(a.cstr, a.cstr_len);
                ofs << " ";
                break;
            case LogArgType::Pointer:
                ofs << a.ptr << " ";
                break;
            }
        }
        ofs << "\n";
        // Uncomment for immediate disk persistence (increase latency):
        // ofs.flush();
    }

    std::ofstream &get_file_stream(uint32_t log_id)
    {
        std::lock_guard<std::mutex> lock(_file_map_mutex);
        auto it = _file_map.find(log_id);
        if (it != _file_map.end())
            return *(it->second);

        // Open new file for unseen log_id
        auto ofs = new std::ofstream();
        std::ostringstream filename;
        filename << "log_" << log_id << ".log";
        ofs->open(filename.str(), std::ios::out | std::ios::app);
        ofs->rdbuf()->pubsetbuf(nullptr, 0); // Unbuffered (tune as needed)
        _file_map[log_id] = ofs;
        return *ofs;
    }

    void flush_all_files()
    {
        std::lock_guard<std::mutex> lock(_file_map_mutex);
        for (auto &kv : _file_map)
        {
            kv.second->flush();
            kv.second->close();
            delete kv.second;
        }
        _file_map.clear();
    }

    std::unordered_map<uint32_t, std::ofstream *> _file_map;
    std::mutex _file_map_mutex;
    std::atomic<bool> _stop_flag{false};
};
