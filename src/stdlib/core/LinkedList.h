#pragma once
// SukiCode LinkedList<T> - 双向链表
// Doubly-linked list.

#include <cstddef>
#include <stdexcept>

namespace suki::stdlib {

template<typename T>
class LinkedList {
private:
    struct Node {
        T value;
        Node* prev = nullptr;
        Node* next = nullptr;
        Node(const T& v) : value(v) {}
        Node(T&& v) : value(std::move(v)) {}
    };

public:
    LinkedList() = default;
    ~LinkedList() { clear(); }

    // 禁止拷贝 / No copy
    LinkedList(const LinkedList&) = delete;
    LinkedList& operator=(const LinkedList&) = delete;

    // 允许移动 / Allow move
    LinkedList(LinkedList&& other) noexcept
        : head_(other.head_), tail_(other.tail_), size_(other.size_) {
        other.head_ = nullptr;
        other.tail_ = nullptr;
        other.size_ = 0;
    }

    LinkedList& operator=(LinkedList&& other) noexcept {
        if (this != &other) {
            clear();
            head_ = other.head_;
            tail_ = other.tail_;
            size_ = other.size_;
            other.head_ = nullptr;
            other.tail_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }

    // 头部插入 / Insert at front
    void insertFront(const T& value) {
        Node* node = new Node(value);
        node->next = head_;
        if (head_) head_->prev = node;
        head_ = node;
        if (!tail_) tail_ = node;
        size_++;
    }

    // 尾部插入 / Insert at back
    void insertBack(const T& value) {
        Node* node = new Node(value);
        node->prev = tail_;
        if (tail_) tail_->next = node;
        tail_ = node;
        if (!head_) head_ = node;
        size_++;
    }

    // 移除头部 / Remove front
    T removeFront() {
        if (isEmpty()) throw std::runtime_error("list is empty");
        Node* node = head_;
        T value = std::move(node->value);
        head_ = node->next;
        if (head_) head_->prev = nullptr;
        else tail_ = nullptr;
        delete node;
        size_--;
        return value;
    }

    // 移除尾部 / Remove back
    T removeBack() {
        if (isEmpty()) throw std::runtime_error("list is empty");
        Node* node = tail_;
        T value = std::move(node->value);
        tail_ = node->prev;
        if (tail_) tail_->next = nullptr;
        else head_ = nullptr;
        delete node;
        size_--;
        return value;
    }

    // 查看头部 / Peek front
    const T& front() const {
        if (isEmpty()) throw std::runtime_error("list is empty");
        return head_->value;
    }

    // 查看尾部 / Peek back
    const T& back() const {
        if (isEmpty()) throw std::runtime_error("list is empty");
        return tail_->value;
    }

    // 大小 / Size
    size_t count() const { return size_; }
    bool isEmpty() const { return size_ == 0; }

    // 清空 / Clear
    void clear() {
        while (head_) {
            Node* next = head_->next;
            delete head_;
            head_ = next;
        }
        tail_ = nullptr;
        size_ = 0;
    }

    // 迭代器 / Iterator
    class Iterator {
    public:
        Iterator(Node* node) : node_(node) {}
        T& operator*() { return node_->value; }
        const T& operator*() const { return node_->value; }
        Iterator& operator++() { node_ = node_->next; return *this; }
        bool operator!=(const Iterator& other) const { return node_ != other.node_; }
        bool operator==(const Iterator& other) const { return node_ == other.node_; }
    private:
        Node* node_;
    };

    Iterator begin() { return Iterator(head_); }
    Iterator end() { return Iterator(nullptr); }
    Iterator begin() const { return Iterator(head_); }
    Iterator end() const { return Iterator(nullptr); }

private:
    Node* head_ = nullptr;
    Node* tail_ = nullptr;
    size_t size_ = 0;
};

} // namespace suki::stdlib
