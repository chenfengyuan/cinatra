
#pragma once

#include "response.hpp"
#include "request_parser.hpp"
#include "logging.hpp"

#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/format.hpp>

#include <memory>
#include <string>
#include <functional>
#include <iostream>
#include <fstream>


namespace cinatra
{
	typedef std::function<bool(const Request&, Response&)> request_handler_t;
	typedef std::function<bool(int,const std::string&, const Request&, Response&)> error_handler_t;

	class Connection
		: public std::enable_shared_from_this<Connection>
	{
	public:
		Connection(boost::asio::io_service& service, const request_handler_t& request_handler,
			const error_handler_t& error_handler, const std::string& public_dir)
			:service_(service), socket_(service), request_handler_(request_handler),
			error_handler_(error_handler), public_dir_(public_dir)
		{
			LOG_DBG << "New connection";
		}
		~Connection()
		{
			boost::system::error_code ec;
			socket_.close(ec);
			LOG_DBG << "Connection closed.";
		}

		boost::asio::ip::tcp::socket& socket(){ return socket_; }

		void start()
		{
			boost::asio::spawn(service_,
				std::bind(&Connection::do_work,
				shared_from_this(), std::placeholders::_1));
		}

	private:
		void do_work(const boost::asio::yield_context& yield)
		{
			//FIXME: 拆分成多个子函数..
			for (;;)
			{
				try
				{
					std::array<char, 8192> buffer;
					RequestParser parser;

					std::size_t total_size = 0;
					for (;;)
					{
						std::size_t n = socket_.async_read_some(boost::asio::buffer(buffer), yield);
						total_size += n;
						if (total_size > 2 * 1024 * 1024)
						{
							throw std::runtime_error("Request toooooooo large");
						}
						auto ret = parser.parse(buffer.data(), buffer.data() + n);
						if (ret == RequestParser::good)
						{
							break;
						}
						if (ret == RequestParser::bad)
						{
							throw std::runtime_error("HTTP Parser error");
						}
					}

					/*
					如果是http1.0，规则是这样的：
					如果request里面的connection是keep-alive，那就说明浏览器想要长连接，服务器如果也同意长连接，
					那么返回的response的connection也应该有keep-alive通知浏览器，如果不想长链接，response里面就不应该有keep-alive;
					如果是1.1的，规则是这样的：
					如果request里面的connection是close，那就说明浏览器不希望长连接，如果没有close，就是默认保持长链接，
					本来是跟keep-alive没关系，但是如果浏览器发了keep-alive，那你返回的时候也应该返回keep-alive;
					惯例是根据有没有close判断是否长链接，但是如果没有close但是有keep-alive，你response也得加keep-alive;
					如果没有close也没有keep-alive
					那就是长链接但是不用返回keep-alive
					*/

					Request req = parser.get_request();
					Response res;
					LOG_DBG << "New request,path:" << req.path();

					auto self = shared_from_this();
					res.direct_write_func_ = 
						[&yield, self, this]
					(const char* data, std::size_t len)->bool
					{
						boost::system::error_code ec;
						boost::asio::async_write(socket_, boost::asio::buffer(data, len), yield[ec]);
						if (ec)
						{
							// TODO: log ec.message().
							std::cout << "direct_write_func error" << ec.message() << std::endl;
							return false;
						}
						return true;
					};

					// 是否已经处理了这个request.
					bool found = false;

					bool keep_alive{};
					bool close_connection{};
					if (parser.check_version(1, 0))
					{
						// HTTP/1.0
						LOG_DBG << "http/1.0";
						if (req.header().val_ncase_equal("Connetion", "Keep-Alive"))
						{
							LOG_DBG << "Keep-Alive";
							keep_alive = true;
							close_connection = false;
						}
						else
						{
							keep_alive = false;
							close_connection = true;
						}

						res.set_version(1, 0);
					}
					else if (parser.check_version(1, 1))
					{
						// HTTP/1.1
						LOG_DBG << "http/1.1";
						if (req.header().val_ncase_equal("Connetion", "close"))
						{
							keep_alive = false;
							close_connection = true;
						}
						else if (req.header().val_ncase_equal("Connetion", "Keep-Alive"))
						{
							keep_alive = true;
							close_connection = false;
						}
						else
						{
							keep_alive = false;
							close_connection = false;
						}

						if (req.header().get_count("host") == 0)
						{
							found = error_handler_(400,"", req, res);
						}

						res.set_version(1, 1);
					}
					else
					{
						LOG_DBG << "Unsupported http version";
						found = error_handler_(400, "Unsupported HTTP version.", req, res);
					}

					if (!found && request_handler_)
					{
						found = request_handler_(req, res);
					}
					if (!found && response_file(req, keep_alive, yield))
					{
						continue;
					}

					//如果都没有找到，404
					if (!found)
					{
						LOG_DBG << "404 Not found";
						error_handler_(404, "", req, res);
					}

					if (keep_alive)
					{
						res.header.add("Connetion", "Keep-Alive");
					}

					//用户没有指定Content-Type，默认设置成text/html
					if (res.header.get_count("Content-Type") == 0)
					{
						res.header.add("Content-Type", "text/html");
					}

					if (!res.is_complete_)
					{
						res.end();
					}

					if (!res.is_chunked_encoding_)
					{
						// 如果是chunked编码数据应该都发完了.
						std::string header_str = res.get_header_str();
						boost::asio::async_write(socket_, boost::asio::buffer(header_str), yield);
						boost::asio::async_write(socket_, res.buffer_, 
							boost::asio::transfer_exactly(res.buffer_.size()), yield);
					}

					if (close_connection)
					{
						shutdown();
					}
				}
				catch (boost::system::system_error& e)
				{
					//网络通信异常，关socket.
					if (e.code() == boost::asio::error::eof)
					{
						LOG_DBG << "Socket shutdown";
					}
					else
					{
						LOG_DBG << "Network exception: " << e.code().message();
					}
					boost::system::error_code ignored_ec;
					socket_.close(ignored_ec);
					return;
				}
				catch (std::exception& e)
				{
					LOG_ERR << "Error occurs,response 500: " << e.what();
					response_5xx(e.what(), yield);
				}
				catch (...)
				{
					response_5xx("", yield);
				}
			}
		}

		bool response_file(Request& req, bool keep_alive, const boost::asio::yield_context& yield)
		{
			std::string path = public_dir_ + req.path();
			std::fstream in(path, std::ios::binary | std::ios::in);
			if (!in)
			{
				return false;
			}

			LOG_DBG << "Response a file";
			in.seekg(0, std::ios::end);

			std::string header =
				boost::str(boost::format(
				"HTTP/1.1 200 OK\r\n"
				"Server: cinatra/0.1\r\n"
				"Date: %1%\r\n"
				"Content-Type: %2%\r\n"
				"Content-Length: %3%\r\n"
				)
				% header_date_str()
				% content_type(path)
				% in.tellg());

			if (keep_alive)
			{
				header += "Connection: Keep-Alive\r\n";
			}

			header += "\r\n";
			in.seekg(0, std::ios::beg);

			boost::asio::async_write(socket_, boost::asio::buffer(header), yield);
			std::vector<char> data(1024 * 1024);
			while (!in.eof())
			{
				in.read(&data[0], data.size());
				//FIXME: warning C4244: “参数”: 从“std::streamoff”转换到“unsigned int”，可能丢失数据.
				boost::asio::async_write(socket_, boost::asio::buffer(data, size_t(in.gcount())), yield);
			}

			return true;
		}

		void shutdown()
		{
			LOG_DBG << "Shutdown connection";
			boost::system::error_code ignored_ec;
			socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored_ec);
		}

		void response_5xx(const std::string& msg, const boost::asio::yield_context& yield)
		{
			Request req;
			Response res;
			error_handler_(500, msg, req, res);
			boost::system::error_code ignored_ec;
			boost::asio::async_write(socket_,
				boost::asio::buffer(res.get_header_str()),
				yield[ignored_ec]);
			boost::asio::async_write(socket_,
				res.buffer_,
				boost::asio::transfer_exactly(res.buffer_.size()),
				yield[ignored_ec]);

			shutdown();
		}
	private:
		boost::asio::io_service& service_;
		boost::asio::ip::tcp::socket socket_;
		const request_handler_t& request_handler_;
		const error_handler_t& error_handler_;
		const std::string& public_dir_;
	};
}
