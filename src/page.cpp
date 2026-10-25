#include "page.h"

Page::Page()
    : page_id_(INVALID_PAGE_ID),
      pin_count_(0),
      is_dirty_(false)
{
    ResetMemory();
#if defined(__unix__) || defined(__APPLE__)
    pthread_rwlock_init(&latch_, nullptr);
#endif
}

Page::~Page()
{
#if defined(__unix__) || defined(__APPLE__)
    pthread_rwlock_destroy(&latch_);
#endif
}

// Getters

page_id_t Page::GetPageId() const
{
    return page_id_.load();
}

int Page::GetPinCount() const
{
    return pin_count_.load();
}

bool Page::IsDirty() const
{
    return is_dirty_.load();
}

char *Page::GetData()
{
    return data_.data();
}

const char *Page::GetData() const
{
    return data_.data();
}

lsn_t Page::GetLSN() const
{
    return *reinterpret_cast<const lsn_t *>(data_.data() + OFFSET_LSN);
}

void Page::SetLSN(lsn_t lsn)
{
    *reinterpret_cast<lsn_t *>(data_.data() + OFFSET_LSN) = lsn;
}

void Page::ResetMemory()
{
    data_.fill(0);
}

// Setters

void Page::SetPageId(page_id_t id)
{
    page_id_.store(id);
}

void Page::SetDirty(bool dirty)
{
    is_dirty_.store(dirty);
}

// Pin Management

void Page::Pin()
{
    ++pin_count_;
}

void Page::Unpin()
{
    int current = pin_count_.load();
    while (current > 0 && !pin_count_.compare_exchange_weak(current, current - 1))
    {
    }
}

// Latching

void Page::WLatch()
{
#if defined(__unix__) || defined(__APPLE__)
    pthread_rwlock_wrlock(&latch_);
#else
    latch_.lock();
#endif
}

void Page::WUnlatch()
{
#if defined(__unix__) || defined(__APPLE__)
    pthread_rwlock_unlock(&latch_);
#else
    latch_.unlock();
#endif
}

void Page::RLatch()
{
#if defined(__unix__) || defined(__APPLE__)
    pthread_rwlock_rdlock(&latch_);
#else
    latch_.lock_shared();
#endif
}

void Page::RUnlatch()
{
#if defined(__unix__) || defined(__APPLE__)
    pthread_rwlock_unlock(&latch_);
#else
    latch_.unlock_shared();
#endif
}