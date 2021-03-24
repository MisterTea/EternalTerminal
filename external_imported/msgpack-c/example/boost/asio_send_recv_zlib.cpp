// MessagePack for C++ example
//
// Copyright (C) 2017 KONDO Takatoshi
//
//    Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//    http://www.boost.org/LICENSE_1_0.txt)
//

#include <string>
#include <sstream>
#include <iostream>

#include <boost/asio.hpp>
#include <boost/lexical_cast.hpp>

#include <msgpack.hpp>
#include <msgpack/zbuffer.hpp>

#include <zlib.h>

void print(std::string const& buf) {
    for (std::string::const_iterator it = buf.begin(), end = buf.end();
         it != end;
         ++it) {
        std::cout
            << std::setw(2)
            << std::hex
            << std::setfill('0')
            << (static_cast<int>(*it) & 0xff)
            << ' ';
    }
    std::cout << std::dec << std::endl;
}

int main() {
    boost::asio::io_service ios;
    std::uint16_t const port = 12345;

    int num_of_zlib_data = 2;
    int idx_zlib_data = 0;

    // Server
    std::size_t const window_size = 11;
    boost::asio::ip::tcp::acceptor ac(ios, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), port));
    boost::asio::ip::tcp::socket ss(ios);
    std::function<void()> do_accept;
    std::function<void()> do_async_read_some;

    // zlib for decompress
    z_stream strm;
    auto zlib_init = [&] {
        strm.zalloc = Z_NULL;
        strm.zfree = Z_NULL;
        strm.opaque = Z_NULL;
        strm.next_in = Z_NULL;
        {
            int zret = inflateInit(&strm);
            if (zret != Z_OK) {
                std::cout << "Zlib inflateInit() error = " << zret << std::endl;
            }
        }
    };
    zlib_init();
    std::vector<char> buf(4); // buf size

    msgpack::unpacker unp;

    do_accept = [&] {
        ac.async_accept(
            ss,
            [&]
            (boost::system::error_code const& e) {
                if (e) {
                    std::cout << __LINE__ << ":" << e.message() << std::endl;
                    return;
                }
                do_async_read_some = [&] {
                    ss.async_read_some(
                        boost::asio::buffer(buf),
                        [&](boost::system::error_code const& e, std::size_t bytes_transferred) {
                            if (e) {
                                std::cout << __LINE__ << ":" << e.message() << std::endl;
                                return;
                            }
                            std::cout << bytes_transferred << " bytes read." << std::endl;
                            print(std::string(std::string(&buf[0], buf.size())));
                            strm.avail_in = bytes_transferred;
                            do {
                                strm.next_in = reinterpret_cast<unsigned char*>(&buf[0]) + (bytes_transferred - strm.avail_in);
                                int zret;
                                unp.reserve_buffer(window_size);
                                strm.avail_out = window_size;
                                strm.next_out = reinterpret_cast<unsigned char*>(unp.buffer());
                                do {
                                    zret = inflate(&strm, Z_NO_FLUSH);
                                    assert(zret != Z_STREAM_ERROR);
                                    switch (zret) {
                                    case Z_NEED_DICT:
                                        zret = Z_DATA_ERROR;
                                        // fall through
                                    case Z_DATA_ERROR:
                                    case Z_MEM_ERROR:
                                        inflateEnd(&strm);
                                        std::cout << "Zlib inflate() error = " << zret << std::endl;
                                        std::exit(-1);
                                    }
                                    std::size_t decompressed_size = window_size - strm.avail_out;
                                    std::cout << decompressed_size << " bytes decompressed." << std::endl;
                                    unp.buffer_consumed(decompressed_size);
                                    msgpack::object_handle oh;
                                    while (unp.next(oh)) {
                                        std::cout << oh.get() << std::endl;
                                    }
                                } while (strm.avail_out == 0);
                                if (zret == Z_STREAM_END) {
                                    inflateEnd(&strm);
                                    std::cout << "Zlib decompress finished." << std::endl;
                                    ++idx_zlib_data;
                                    if (idx_zlib_data == num_of_zlib_data) {
                                        std::cout << "All zlib decompress finished." << std::endl;
                                        return;
                                    }
                                    zlib_init();
                                }
                            } while (strm.avail_in != 0);
                            do_async_read_some();
                        }
                    );
                };
                do_async_read_some();
            }
        );
    };
    do_accept();

    // Client
    auto host = "localhost";

    boost::asio::ip::tcp::resolver r(ios);

#if BOOST_VERSION < 106600
    boost::asio::ip::tcp::resolver::query q(host, boost::lexical_cast<std::string>(port));
    auto it = r.resolve(q);
    boost::asio::ip::tcp::resolver::iterator end;
#else  // BOOST_VERSION < 106600
    auto eps = r.resolve(host, boost::lexical_cast<std::string>(port));
    auto it = eps.begin();
    auto end = eps.end();
#endif // BOOST_VERSION < 106600

    boost::asio::ip::tcp::socket cs(ios);
    boost::asio::async_connect(
        cs,
        it,
        end,
        [&]
        (boost::system::error_code const& e, boost::asio::ip::tcp::resolver::iterator) {
            if (e) {
                std::cout << __LINE__ << ":" << e.message() << std::endl;
                return;
            }
            std::cout << __LINE__ << ":client connected" << std::endl;
            for (int i = 0; i != num_of_zlib_data; ++i) {
                msgpack::zbuffer zb;
                msgpack::pack(zb, std::make_tuple(i, false, "hello world", 12.3456));
                zb.flush(); // finalize zbuffer (don't forget it)
                print(std::string(zb.data(), zb.size()));
                write(cs, boost::asio::buffer(zb.data(), zb.size()));
            }
        }
    );

    // Start
    ios.run();
}
