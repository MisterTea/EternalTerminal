// MessagePack for C++ example
//
// Copyright (C) 2017 KONDO Takatoshi
//
//    Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//    http://www.boost.org/LICENSE_1_0.txt)
//

#include <iostream>
#include <sstream>
#include <cassert>
#include <thread>

// MSGPACK_USE_X3_PARSE should be defined before including msgpack.hpp
// It usually defined as a compiler option as -DMSGPACK_USE_X3_PARSE.

//#define MSGPACK_USE_X3_PARSE

#include <msgpack.hpp>

#include <boost/asio.hpp>
#include <boost/coroutine2/all.hpp>

#if defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif // defined(__clang__)

#include <boost/spirit/home/support/multi_pass.hpp>

#if defined(__clang__)
#pragma GCC diagnostic pop
#endif // defined(__clang__)

namespace as = boost::asio;
namespace x3 = boost::spirit::x3;
namespace coro2 = boost::coroutines2;

using pull_type = coro2::asymmetric_coroutine<std::shared_ptr<std::vector<char>>>::pull_type;

// iterator fetching data from coroutine2.
class buffered_iterator : public std::iterator<std::input_iterator_tag, char> {
public:
    using pointer_t = typename iterator::pointer;
    using reference_t = typename iterator::reference;

    explicit buffered_iterator(pull_type& source) noexcept
        : source_{ &source } {
        fetch_();
    }
    buffered_iterator() = default;

    bool operator==(buffered_iterator const& other) const noexcept {
        if (!other.source_ && !source_ && !other.buf_ && !buf_) return true;
        return other.it_ == it_;
    }

    bool operator!=(buffered_iterator const& other) const noexcept {
        return !(other == *this);
    }

    buffered_iterator & operator++() {
        increment_();
        return * this;
    }

    buffered_iterator operator++(int) = delete;

    reference_t operator*() noexcept {
        return *it_;
    }

    pointer_t operator->() noexcept {
        return std::addressof(*it_);
    }

private:
    void fetch_() noexcept {
        BOOST_ASSERT( nullptr != source_);
        if (*source_) {
            buf_ = source_->get();
            it_ = buf_->begin();
        }
        else {
            source_ = nullptr;
            buf_.reset();
        }
    }

    void increment_() {
        BOOST_ASSERT( nullptr != source_);
        BOOST_ASSERT(*source_);
        if (++it_ == buf_->end()) {
            (*source_)();
            fetch_();
        }
    }

private:
    pull_type* source_{ nullptr };
    std::shared_ptr<std::vector<char>> buf_;
    std::vector<char>::iterator it_;
};

// session class that corresponding to each client
class session : public std::enable_shared_from_this<session> {
public:
    session(as::ip::tcp::socket socket)
        : socket_(std::move(socket)) {
    }

    void start() {
        sink_ = std::make_shared<coro2::asymmetric_coroutine<std::shared_ptr<std::vector<char>>>::push_type>(
            [&, this](pull_type& source) {
                // *1 is started when the first sink is called.

                std::cout << "session started" << std::endl;
                do_read();
                source();

                // use buffered_iterator here
                // b is incremented in msgpack::unpack() and fetch data from sink
                // via coroutine2 mechanism
                auto b = boost::spirit::make_default_multi_pass(buffered_iterator(source));
                auto e = boost::spirit::make_default_multi_pass(buffered_iterator());

                // This is usually an infinity look, but for test, loop is finished when
                // two message pack data is processed.
                for (int i = 0; i != 2; ++i) {
                    auto oh = msgpack::unpack(b, e);
                    std::cout << oh.get() << std::endl;
                }
            }
        );
        // send dummy data to start *1
        (*sink_)({});
    }

private:
    void do_read() {
        std::cout << "session do_read() is called" << std::endl;
        auto self(shared_from_this());
        auto data = std::make_shared<std::vector<char>>(static_cast<std::size_t>(max_length));
        socket_.async_read_some(
            boost::asio::buffer(*data),
            [this, self, data]
            (boost::system::error_code ec, std::size_t length) {
                if (!ec) {
                    data->resize(length);
                    (*sink_)(data);
                    do_read();
                }
            }
        );
    }

    as::ip::tcp::socket socket_;
    static constexpr std::size_t const max_length = 1024;
    std::shared_ptr<coro2::asymmetric_coroutine<std::shared_ptr<std::vector<char>>>::push_type> sink_;
};

class server {
public:
    server(
        as::io_service& ios,
        std::uint16_t port)
        : acceptor_(ios, as::ip::tcp::endpoint(as::ip::tcp::v4(), port)),
          socket_(ios) {
        do_accept();
        std::cout << "server start accept" << std::endl;
        ios.run();
    }

private:
    void do_accept() {
        acceptor_.async_accept(
            socket_,
            [this](boost::system::error_code ec) {
                if (!ec) {
                    std::make_shared<session>(std::move(socket_))->start();
                }
                // for test, only one session is accepted.
                // do_accept();
            }
        );
    }

    as::ip::tcp::acceptor acceptor_;
    as::ip::tcp::socket socket_;
};

int main() {
    std::thread srv(
        []{
            boost::asio::io_service ios;
            server s(ios, 12345);
        }
    );

    std::thread cli(
        []{
            std::this_thread::sleep_for(std::chrono::seconds(1));
            std::cout << "client start" << std::endl;

            std::stringstream ss;
            std::map<std::string, std::vector<int>> v1 {
                { "ABC", { 1, 2, 3 } },
                { "DEFG", { 4, 5 } }
            };
            std::vector<std::string> v2 {
                "HIJ", "KLM", "NOP"
                    };
            msgpack::pack(ss, v1);
            msgpack::pack(ss, v2);

            auto send_data = ss.str();

            boost::asio::io_service ios;
            as::ip::tcp::resolver::query q("127.0.0.1", "12345");
            as::ip::tcp::resolver r(ios);
            auto it = r.resolve(q);

            std::cout << "client connect" << std::endl;
            as::ip::tcp::socket s(ios);
            as::connect(s, it);


            std::size_t const size = 5;
            std::size_t rest = send_data.size();
            std::size_t index = 0;
            while (rest != 0) {
                std::cout << "client send data" << std::endl;
                auto send_size = size < rest ? size : rest;
                as::write(s, as::buffer(&send_data[index], send_size));
                rest -= send_size;
                index += send_size;
                std::cout << "client wait" << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    );

    cli.join();
    std::cout << "client joinded" << std::endl;
    srv.join();
    std::cout << "server joinded" << std::endl;
}
