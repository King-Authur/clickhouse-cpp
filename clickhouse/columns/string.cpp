#include "string.h"
#include "utils.h"

#include <algorithm>

#include "../base/wire_format.h"

namespace clickhouse {

ColumnFixedString::ColumnFixedString(size_t n)
    : Column(Type::CreateString(n))
    , string_size_(n)
{
}

void ColumnFixedString::Append(std::string_view str) {
    if (str.size() > string_size_) {
        throw ValidationError("Expected string of length not greater than "
                                 + std::to_string(string_size_) + " bytes, received "
                                 + std::to_string(str.size()) + " bytes.");
    }

    if (data_.capacity() - data_.size() < str.size())
    {
        // round up to the next block size
        const auto new_size = (((data_.size() + string_size_) / DEFAULT_BLOCK_SIZE) + 1) * DEFAULT_BLOCK_SIZE;
        data_.reserve(new_size);
    }

    data_.insert(data_.size(), str);
    // Pad up to string_size_ with zeroes.
    const auto padding_size = string_size_ - str.size();
    data_.resize(data_.size() + padding_size, char(0));
}

void ColumnFixedString::Clear() {
    data_.clear();
}

std::string_view ColumnFixedString::At(size_t n) const {
    const auto pos = n * string_size_;
    return std::string_view(&data_.at(pos), string_size_);
}

std::string_view ColumnFixedString::operator [](size_t n) const {
    const auto pos = n * string_size_;
    return std::string_view(&data_[pos], string_size_);
}

size_t ColumnFixedString::FixedSize() const
{
       return string_size_;
}

void ColumnFixedString::Append(ColumnRef column) {
    if (auto col = column->As<ColumnFixedString>()) {
        if (string_size_ == col->string_size_) {
            data_.insert(data_.end(), col->data_.begin(), col->data_.end());
        }
    }
}

bool ColumnFixedString::LoadBody(InputStream * input, size_t rows) {
    data_.resize(string_size_ * rows);
    if (!WireFormat::ReadBytes(*input, &data_[0], data_.size())) {
        return false;
    }

    return true;
}

void ColumnFixedString::SaveBody(OutputStream* output) {
    WireFormat::WriteBytes(*output, data_.data(), data_.size());
}

size_t ColumnFixedString::Size() const {
    return data_.size() / string_size_;
}

ColumnRef ColumnFixedString::Slice(size_t begin, size_t len) const {
    auto result = std::make_shared<ColumnFixedString>(string_size_);

    if (begin < Size()) {
        const auto b = begin * string_size_;
        const auto l = len * string_size_;
        result->data_ = data_.substr(b, std::min(data_.size() - b, l));
    }

    return result;
}

ColumnRef ColumnFixedString::CloneEmpty() const {
    return std::make_shared<ColumnFixedString>(string_size_);
}

void ColumnFixedString::Swap(Column& other) {
    auto & col = dynamic_cast<ColumnFixedString &>(other);
    std::swap(string_size_, col.string_size_);
    data_.swap(col.data_);
}

ItemView ColumnFixedString::GetItem(size_t index) const {
    return ItemView{Type::FixedString, this->At(index)};
}

struct ColumnString::Block
{
    using CharT = typename std::string::value_type;

    explicit Block(size_t starting_capacity)
        : size(0),
          capacity(starting_capacity),
          data_(starting_capacity, 0)
    {}
    explicit Block(std::string&& payload)
        : size(payload.size()), capacity(payload.size()), data_(std::move(payload)) {}

    inline auto GetAvailable() const
    {
        return capacity - size;
    }

   std::string_view AppendUnsafe(std::string_view str) {
        std::copy(str.begin(), str.end(), data_.begin() + size);
        std::string_view sv{data_.data() + size, str.size()};
        size += str.size();
        return sv;
    }

    auto GetCurrentWritePos()
    {
        return &data_[size];
    }

    std::string_view ConsumeTailAsStringViewUnsafe(size_t len)
    {
        const auto start = &data_[size];
        size += len;
        return std::string_view(start, len);
    }

    size_t size;
    const size_t capacity;
    std::string data_;
};

ColumnString::ColumnString()
    : Column(Type::CreateString())
{}

ColumnString::ColumnString(const std::vector<std::string>& data)
    : Column(Type::CreateString()) {
    ConstructFromVector(data);
}

ColumnString::ColumnString(const std::vector<std::string_view>& data)
    : Column(Type::CreateString()) {
    ConstructFromVector(data);
}

ColumnString::ColumnString(std::string&& payload, std::vector<std::string_view>&& items)
    : Column(Type::CreateString()), items_(std::move(items)), blocks_() {
    blocks_.emplace_back(std::move(payload));
}

ColumnString::~ColumnString()
{}

void ColumnString::Reserve(size_t rows) {
    items_.reserve(rows);
    blocks_.reserve(rows);
}

void ColumnString::Append(std::string_view str) {
    if (blocks_.size() == 0 || blocks_.back().GetAvailable() < str.length())
    {
        blocks_.emplace_back(std::max(DEFAULT_BLOCK_SIZE, str.size()));
    }

    items_.emplace_back(blocks_.back().AppendUnsafe(str));
}

void ColumnString::AppendUnsafe(std::string_view str)
{
    items_.emplace_back(blocks_.back().AppendUnsafe(str));
}

void ColumnString::Clear() {
    items_.clear();
    blocks_.clear();
}

std::string_view ColumnString::At(size_t n) const {
    return items_.at(n);
}

std::string_view ColumnString::operator [] (size_t n) const {
    return items_[n];
}

void ColumnString::Append(ColumnRef column) {
    if (auto col = column->As<ColumnString>()) {
        const auto total_size = ComputeTotalSize(col->items_);

        // TODO: fill up existing block with some items and then add a new one for the rest of items
        if (blocks_.size() == 0 || blocks_.back().GetAvailable() < total_size)
            blocks_.emplace_back(std::max(DEFAULT_BLOCK_SIZE, total_size));
        items_.reserve(items_.size() + col->Size());

        for (size_t i = 0; i < column->Size(); ++i) {
            this->AppendUnsafe((*col)[i]);
        }
    }
}

bool ColumnString::LoadBody(InputStream* input, size_t rows) {
    items_.clear();
    blocks_.clear();

    items_.reserve(rows);
    Block * block = nullptr;

    // TODO(performance): unroll a loop to a first row (to get rid of `blocks_.size() == 0` check) and the rest.
    for (size_t i = 0; i < rows; ++i) {
        uint64_t len;
        if (!WireFormat::ReadUInt64(*input, &len))
            return false;

        if (blocks_.size() == 0 || len > block->GetAvailable())
            block = &blocks_.emplace_back(std::max<size_t>(DEFAULT_BLOCK_SIZE, len));

        if (!WireFormat::ReadBytes(*input, block->GetCurrentWritePos(), len))
            return false;

        items_.emplace_back(block->ConsumeTailAsStringViewUnsafe(len));
    }

    return true;
}

void ColumnString::SaveBody(OutputStream* output) {
    for (const auto & item : items_) {
        WireFormat::WriteString(*output, item);
    }
}

size_t ColumnString::Size() const {
    return items_.size();
}

ColumnRef ColumnString::Slice(size_t begin, size_t len) const {
    auto result = std::make_shared<ColumnString>();

    if (begin < items_.size()) {
        len = std::min(len, items_.size() - begin);

        result->blocks_.emplace_back(ComputeTotalSize(items_, begin, len));
        for (size_t i = begin; i < begin + len; ++i)
        {
            result->Append(items_[i]);
        }
    }

    return result;
}

ColumnRef ColumnString::CloneEmpty() const {
    return std::make_shared<ColumnString>();
}

void ColumnString::Swap(Column& other) {
    auto & col = dynamic_cast<ColumnString &>(other);
    items_.swap(col.items_);
    blocks_.swap(col.blocks_);
}

ItemView ColumnString::GetItem(size_t index) const {
    return ItemView{Type::String, this->At(index)};
}

}
