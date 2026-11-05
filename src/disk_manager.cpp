#include "disk_manager.h"

#include <cstring>
#include <stdexcept>
#include <mutex>
#include <unordered_map>
#include <memory>

// File-path-keyed mutex registry.
// All DiskManager instances that open the same file path share one mutex,
// so their seek+read/write sequences are correctly serialized.
// Different files remain fully independent.
namespace {

std::mutex registry_mutex_;
std::unordered_map<std::string, std::weak_ptr<std::mutex>> file_mutexes_;

std::shared_ptr<std::mutex> GetFileMutex(const std::string& path) {
    std::lock_guard<std::mutex> reg_lock(registry_mutex_);
    for (auto it = file_mutexes_.begin(); it != file_mutexes_.end(); ) {
        if (it->second.expired()) {
            it = file_mutexes_.erase(it);
        } else {
            ++it;
        }
    }
    auto& weak = file_mutexes_[path];
    auto shared = weak.lock();
    if (!shared) {
        shared = std::make_shared<std::mutex>();
        weak = shared;
    }
    return shared;
}

}  // namespace

DiskManager::DiskManager(const std::string &file_name)
    : file_name_(file_name), next_page_id_(0),
      file_mutex_(GetFileMutex(file_name))
{
    db_file_.open(file_name,
                  std::ios::binary |
                  std::ios::in |
                  std::ios::out);

    if (!db_file_.is_open())
    {
        std::ofstream create(file_name, std::ios::binary);

        if (!create.is_open())
        {
            throw std::runtime_error("Failed to create database file: " + file_name);
        }

        create.close();

        db_file_.open(file_name,
                      std::ios::binary |
                      std::ios::in |
                      std::ios::out);

        if (!db_file_.is_open())
        {
            throw std::runtime_error("Failed to open database file: " + file_name);
        }
    }

    db_file_.seekg(0, std::ios::end);

    std::streamoff size = db_file_.tellg();

    if (size == -1)
    {
        throw std::runtime_error("Failed to determine database file size.");
    }

    if (size % PAGE_SIZE != 0)
    {
        throw std::runtime_error("Database file is corrupted.");
    }

    next_page_id_ = static_cast<page_id_t>(size / PAGE_SIZE);
}

page_id_t DiskManager::AllocatePage()
{
    std::lock_guard<std::mutex> lock(*file_mutex_);
    return next_page_id_++;
}

page_id_t DiskManager::GetNextPageId() const
{
    std::lock_guard<std::mutex> lock(*file_mutex_);
    return next_page_id_;
}

void DiskManager::WritePage(page_id_t page_id, const char *data)
{

     std::lock_guard<std::mutex> lock(*file_mutex_);

    const size_t offset = static_cast<size_t>(page_id) * PAGE_SIZE;

    // Use the write pointer to determine current file size.
    // Using seekp/tellp keeps read and write positions conceptually separate.
    db_file_.seekp(0, std::ios::end);

    if (!db_file_)
    {
        throw std::runtime_error("Failed to seek to end of file.");
    }

    std::streamoff file_size = db_file_.tellp();

    if (file_size == -1)
    {
        throw std::runtime_error("Failed to determine file size.");
    }

    if (file_size % PAGE_SIZE != 0)
    {
        throw std::runtime_error("Corrupted database file.");
    }

    char zero_page[PAGE_SIZE] = {};

    while (static_cast<size_t>(file_size) < offset)
    {
        db_file_.write(zero_page, PAGE_SIZE);

        if (!db_file_)
        {
            throw std::runtime_error("Failed while zero-filling pages.");
        }

        file_size += PAGE_SIZE;
    }

    db_file_.seekp(offset);

    if (!db_file_)
    {
        throw std::runtime_error("Failed to seek page.");
    }

    db_file_.write(data, PAGE_SIZE);

    if (!db_file_)
    {
        throw std::runtime_error("Failed to write page.");
    }

    db_file_.flush();

    if (!db_file_)
    {
        throw std::runtime_error("Failed to flush file.");
    }

    num_writes_.fetch_add(1, std::memory_order_relaxed);
}

void DiskManager::ReadPage(page_id_t page_id, char *buffer)
{

    std::lock_guard<std::mutex> guard(*file_mutex_);

    std::memset(buffer, 0, PAGE_SIZE);

    size_t offset = static_cast<size_t>(page_id) * PAGE_SIZE;

    db_file_.seekg(offset);

    if (!db_file_)
    {
        throw std::runtime_error("Failed to seek.");
    }

    db_file_.read(buffer, PAGE_SIZE);

    std::streamsize bytes_read = db_file_.gcount();

    if (bytes_read < static_cast<std::streamsize>(PAGE_SIZE))
    {
        std::memset(buffer + bytes_read, 0, PAGE_SIZE - bytes_read);

        db_file_.clear();
    }

    num_reads_.fetch_add(1, std::memory_order_relaxed);
}

DiskManager::~DiskManager()
{
    if (file_mutex_)
    {
        std::lock_guard<std::mutex> guard(*file_mutex_);
        if (db_file_.is_open())
        {
            db_file_.close();
        }
    }
}