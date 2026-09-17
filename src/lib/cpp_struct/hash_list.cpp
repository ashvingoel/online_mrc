#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>

#include "cpp_struct/hash_list.hpp"
#include "logger/logger.h"
#include <glib.h>

constexpr bool DEBUG = false;

void
HashList::append(ListNode *node, bool const add_to_map)
{
    LOGGER_TRACE("append(%zu)", node->key);
    validate();
    if (add_to_map) {
        map_.emplace(node->key, node);
    }
    if (tail_ == nullptr) {
        head_ = tail_ = node;
        node->sanitize();
        validate();
        return;
    }
    g_assert_nonnull(head_);
    g_assert_null(tail_->r);
    g_assert_cmpuint(map_.size(), !=, 0);
    tail_->r = node;
    node->l = tail_;
    tail_ = node;
    validate();
}

std::pair<std::unordered_map<uint64_t, ListNode *const>::iterator, ListNode *>
HashList::extract(uint64_t const key)
{
    auto it = map_.find(key);
    if (it == map_.end()) {
        validate();
        return {it, nullptr};
    }
    ListNode *const n = it->second;
    if (n->l != nullptr) {
        n->l->r = n->r;
    } else {
        head_ = n->r;
    }
    if (n->r != nullptr) {
        n->r->l = n->l;
    } else {
        tail_ = n->l;
    }
    // Reset internal pointers so we don't dangle invalid pointers.
    n->sanitize();
    return {it, n};
}

HashList::HashList() {}

HashList::~HashList()
{
    ListNode *next = nullptr;
    for (auto p = head_; p != nullptr; p = next) {
        next = p->r;
        p->reset();
        std::free(p);
    }
}

void
HashList::validate() const
{
    if (!DEBUG) {
        return;
    }
    // Sanity checks
    switch (map_.size()) {
    case 0:
        g_assert_null(head_);
        g_assert_null(tail_);
        break;
    case 1:
        g_assert_nonnull(head_);
        g_assert_nonnull(tail_);
        g_assert_cmphex((guint64)head_, ==, (guint64)tail_);
        break;
    default:
        g_assert_nonnull(head_);
        g_assert_nonnull(tail_);
        g_assert_cmphex((guint64)head_, !=, (guint64)tail_);
        break;
    }

    // Check internal consistency of the data structure.
    size_t cnt = 0;
    for (auto p = head_; p != nullptr; p = p->r) {
        g_assert_true(map_.contains(p->key));
        g_assert_true(map_.at(p->key) == p);
        ++cnt;
        if (p->l != nullptr) {
            g_assert_cmphex((guint64)p->l->r, ==, (guint64)p);
        } else {
            g_assert_cmphex((guint64)p, ==, (guint64)head_);
        }
        if (p->r != nullptr) {
            g_assert_cmphex((guint64)p->r->l, ==, (guint64)p);
        } else {
            g_assert_cmphex((guint64)tail_, ==, (guint64)p);
        }
    }
    g_assert_cmpuint(cnt, ==, map_.size());
}

void
HashList::debug_print() const
{
    std::cout << "- Map(" << map_.size() << "): ";
    for (auto [k, p] : map_) {
        std::cout << k << ": " << p << ", ";
    }
    std::cout << std::endl
              << "- Head: " << head_ << std::endl
              << "- Tail: " << tail_ << std::endl
              << "- HashList: ";
    for (auto p = head_; p != nullptr; p = p->r) {
        std::cout << p << ": " << p->key << ", ";
    }
    std::cout << std::endl;
}

ListNodeIterator
HashList::begin() const
{
    return ListNodeIterator{head_};
}

ListNodeIterator
HashList::end() const
{
    return ListNodeIterator{nullptr};
}

std::optional<uint64_t>
HashList::front() const
{
    return head_ ? std::optional{head_->key} : std::nullopt;
}

std::optional<uint64_t>
HashList::back() const
{
    return tail_ ? std::optional{tail_->key} : std::nullopt;
}

size_t
HashList::size() const
{
    return map_.size();
}

bool
HashList::contains(uint64_t const key) const
{
    return map_.contains(key);
}

ListNode const *
HashList::get(uint64_t const key) const
{
    LOGGER_TRACE("get(%zu)", key);
    validate();
    auto r = map_.find(key);
    if (r == map_.end()) {
        validate();
        return nullptr;
    }
    validate();
    return r->second;
}

void
HashList::access(uint64_t const key)
{
    LOGGER_TRACE("access(%zu)", key);
    validate();
    auto [it, n] = extract(key);
    if (n == nullptr) {
        assert(it == map_.end());
        n = (ListNode *)std::malloc(sizeof(ListNode));
        if (n == nullptr) {
            std::perror(strerror(errno));
            std::exit(1);
        }
        *n = ListNode{key, nullptr, nullptr};
        append(n, true);
        validate();
    } else {
        append(n, false);
        validate();
    }
    validate();
}

bool
HashList::remove(uint64_t const key)
{
    LOGGER_TRACE("remove(%zu)", key);
    validate();
    auto [it, n] = extract(key);
    if (n == nullptr) {
        validate();
        return false;
    }
    map_.erase(it);
    std::free(n);
    validate();
    return true;
}

ListNode *
HashList::extract_head()
{
    LOGGER_TRACE("extract_head() -> %p(%s)",
                 head_,
                 head_ ? std::to_string(head_->key).c_str() : "?");
    validate();
    if (head_ == nullptr) {
        validate();
        return nullptr;
    }
    auto [it, n] = extract(head_->key);
    map_.erase(it);
    validate();
    return n;
}
