#pragma once
// SukiCode Matrix<T> - 矩阵
// Matrix for numeric computing.

#include <vector>
#include <cstddef>
#include <stdexcept>

namespace suki::stdlib {

template<typename T>
class Matrix {
public:
    Matrix(size_t rows, size_t cols, const T& defaultValue = T{})
        : rows_(rows), cols_(cols), data_(rows * cols, defaultValue) {}

    // 元素访问 / Element access
    T& at(size_t row, size_t col) {
        if (row >= rows_ || col >= cols_) throw std::out_of_range("matrix index out of range");
        return data_[row * cols_ + col];
    }

    const T& at(size_t row, size_t col) const {
        if (row >= rows_ || col >= cols_) throw std::out_of_range("matrix index out of range");
        return data_[row * cols_ + col];
    }

    // 维度 / Dimensions
    size_t rows() const { return rows_; }
    size_t cols() const { return cols_; }

    // 矩阵加法 / Matrix addition
    Matrix operator+(const Matrix& other) const {
        if (rows_ != other.rows_ || cols_ != other.cols_) {
            throw std::runtime_error("matrix dimensions mismatch");
        }
        Matrix result(rows_, cols_);
        for (size_t i = 0; i < data_.size(); i++) {
            result.data_[i] = data_[i] + other.data_[i];
        }
        return result;
    }

    // 矩阵乘法 / Matrix multiplication
    Matrix operator*(const Matrix& other) const {
        if (cols_ != other.rows_) {
            throw std::runtime_error("matrix dimensions mismatch for multiplication");
        }
        Matrix result(rows_, other.cols_, T{});
        for (size_t i = 0; i < rows_; i++) {
            for (size_t j = 0; j < other.cols_; j++) {
                T sum = T{};
                for (size_t k = 0; k < cols_; k++) {
                    sum += at(i, k) * other.at(k, j);
                }
                result.at(i, j) = sum;
            }
        }
        return result;
    }

    // 标量乘法 / Scalar multiplication
    Matrix operator*(const T& scalar) const {
        Matrix result(rows_, cols_);
        for (size_t i = 0; i < data_.size(); i++) {
            result.data_[i] = data_[i] * scalar;
        }
        return result;
    }

    // 转置 / Transpose
    Matrix transposed() const {
        Matrix result(cols_, rows_);
        for (size_t i = 0; i < rows_; i++) {
            for (size_t j = 0; j < cols_; j++) {
                result.at(j, i) = at(i, j);
            }
        }
        return result;
    }

private:
    size_t rows_;
    size_t cols_;
    std::vector<T> data_;
};

} // namespace suki::stdlib
