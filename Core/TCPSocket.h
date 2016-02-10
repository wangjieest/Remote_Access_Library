#pragma once
#include <memory>
#include "ISocket.h"
#include "IBaseNetworkDriver.h"
#include "Packet.h"
#include <boost\asio.hpp>
#include <boost/asio/deadline_timer.hpp>
#include <boost/asio/ssl.hpp>
#include "zstd.h"
#include "SocketImpl.h"

namespace SL {
	namespace Remote_Access_Library {

		namespace Network {

			typedef boost::asio::ssl::stream<boost::asio::ip::tcp::socket> ssl_socket;
			typedef boost::asio::ip::tcp::socket socket;

			//this class is async so all calls return immediately and are later executed
			template<typename BASESOCKET>class TCPSocket : public ISocket {
			protected:
				BASESOCKET _socket;
				SocketImpl _SocketImpl;
			public:
				static_assert(std::is_same<BASESOCKET, boost::asio::ip::tcp::socket>::value || std::is_same<BASESOCKET, boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>::value, "BASESOCKET must be boost::asio::ip::tcp::socket or boost::asio::ssl::stream<boost::asio::ip::tcp::socket>");

				//MUST BE CREATED AS A SHARED_PTR OTHERWISE IT WILL CRASH!
				explicit TCPSocket(IBaseNetworkDriver* netevents, BASESOCKET& socket) :_socket(std::move(socket)), _SocketImpl(_socket.get_io_service(), netevents) { }
				//MUST BE CREATED AS A SHARED_PTR OTHERWISE IT WILL CRASH!
				explicit TCPSocket(IBaseNetworkDriver* netevents, boost::asio::io_service& io_service) : _socket(io_service), _SocketImpl(io_service, netevents) { }

				virtual ~TCPSocket() {
					close();
				}



				//adds the data to the internal queue, does not block and returns immediately.
				virtual void send(Packet& pack) override {
					auto self(shared_from_this());
					auto compack(std::make_shared<Packet>(std::move(compress(pack))));
					auto beforesize = pack.Payload_Length;
					_socket.get_io_service().post([this, self, compack, beforesize]()
					{
						_SocketImpl.AddOutgoingPacket(compack, beforesize);
						if (!_SocketImpl.writing())
						{
							writeexpire_from_now(30);
							writeheader(_SocketImpl.GetNextWritePacket());
						}
					});
				}
				//sends a request that the socket be closed. NetworkEvents::OnClose will be called when the call is successful
				virtual bool closed() override {
					return _SocketImpl.closed(_socket.lowest_layer());
				}
				virtual void close() override {
					if (_SocketImpl.closed(_socket.lowest_layer())) return;
					_SocketImpl.get_Driver()->OnClose(shared_from_this());
					_SocketImpl.close(_socket.lowest_layer());
				}
				//pending packets which are queued up and waiting to be sent
				virtual SocketStats get_SocketStats() const override { return _SocketImpl.get_Socketstats(); }
				//calling connected with a host and port will attempt an async connection returns immediatly and executes the OnConnect Callback when connected. If connection fails, the OnClose callback is called
				virtual void connect(const char* host, const char* port) override {
					auto self(shared_from_this());
					if (host == nullptr && port == nullptr) {
						handshake();
					}
					else {
						try
						{
							boost::asio::ip::tcp::resolver resolver(_socket.get_io_service());
							boost::asio::ip::tcp::resolver::query query(host, port);
							auto endpoint = resolver.resolve(query);
							boost::asio::async_connect(_socket, endpoint, [self, this](boost::system::error_code ec, boost::asio::ip::tcp::resolver::iterator)
							{
								if (!closed()) {
									_SocketImpl.get_Driver()->OnConnect(self);
									readheader();
								}
								else close();
							});
						}
						catch (...) {
							close();
						}
					}

				}
				virtual SocketTypes get_type() const override { return SocketTypes::TCPSOCKET; }


			protected:


				virtual void handshake()  override {
					auto self(shared_from_this());
					_SocketImpl.get_Driver()->OnConnect(self);
					readheader();
				}
				virtual void readheader()  override {
					readexpire_from_now(30);
					auto self(shared_from_this());
					boost::asio::async_read(_socket, boost::asio::buffer(&_SocketImpl.ReadPacketHeader, sizeof(_SocketImpl.ReadPacketHeader)), [self, this](boost::system::error_code ec, std::size_t len/*length*/)
					{
						if (!ec && !closed()) {
							assert(len == sizeof(_SocketImpl.ReadPacketHeader));
							readbody();
						}
						else close();
					});
				}
				virtual void readbody() override {
					readexpire_from_now(30);
					auto self(shared_from_this());
					auto p(_SocketImpl.get_ReadBuffer());
					auto size(_SocketImpl.get_ReadBufferSize());
					boost::asio::async_read(_socket, boost::asio::buffer(p, size), [self, this](boost::system::error_code ec, std::size_t len/*length*/)
					{
						if (!ec && !closed()) {
							assert(len == _SocketImpl.get_ReadBufferSize());
							_SocketImpl.get_Driver()->OnReceive(self, std::make_shared<Packet>(std::move(decompress(_SocketImpl.GetNextReadPacket()))));
							readheader();
						}
						else close();
					});
				}
				virtual void writeheader(std::shared_ptr<Packet> packet) override {
					auto self(shared_from_this());
					boost::asio::async_write(_socket, boost::asio::buffer(&_SocketImpl.WritePacketHeader, sizeof(_SocketImpl.WritePacketHeader)), [self, this, packet](boost::system::error_code ec, std::size_t byteswritten)
					{
						if (!ec && !closed())
						{
							assert(byteswritten == sizeof(_SocketImpl.WritePacketHeader));
							writebody(packet);
						}
						else close();
					});
				}
				virtual void writebody(std::shared_ptr<Packet> packet) override {
					auto self(shared_from_this());
					boost::asio::async_write(_socket, boost::asio::buffer(packet->Payload, packet->Payload_Length), [self, this, packet](boost::system::error_code ec, std::size_t byteswritten)
					{
						if (!ec && !_SocketImpl.closed(_socket.lowest_layer()))
						{
							assert(byteswritten == packet->Payload_Length);
							if (!_SocketImpl.OutGoingBuffer_empty()) {
								writeheader(_SocketImpl.GetNextWritePacket());
							}
							else {
								_SocketImpl.writing(false);
							}
							writeexpire_from_now(30);
						}
						else close();
					});
				}

				virtual Packet compress(Packet& packet) override
				{
					auto maxsize = ZSTD_compressBound(packet.Payload_Length);
					Packet p(packet.Packet_Type, static_cast<unsigned int>(maxsize), std::move(packet.Header));
					p.Payload_Length = static_cast<unsigned int>(ZSTD_compress(p.Payload, maxsize, packet.Payload, static_cast<unsigned int>(packet.Payload_Length), 3));
					_SocketImpl.UpdateWriteStats(p, packet.Payload_Length);
					return p;
				}
				virtual Packet decompress(Packet& packet)  override
				{
					Packet p(packet.Packet_Type, _SocketImpl.ReadPacketHeader.UncompressedLength, std::move(packet.Header));
					p.Payload_Length = static_cast<unsigned int>(ZSTD_decompress(p.Payload, _SocketImpl.ReadPacketHeader.UncompressedLength, packet.Payload, packet.Payload_Length));
					if (ZSTD_isError(p.Payload_Length) > 0) {
						std::cout << ZSTD_getErrorName(p.Payload_Length) << std::endl;
						return Packet(static_cast<unsigned int>(PACKET_TYPES::INVALID));//empty packet
					}
					_SocketImpl.UpdateReadStats();
					return p;
				}


				void readexpire_from_now(int seconds)
				{
					return;//DONT USE THIS YET
					auto self(shared_from_this());
					_SocketImpl.StartReadTimer(seconds);
					if (seconds >= 0) {
						_SocketImpl.get_read_deadline_timer().async_wait([this, self](boost::system::error_code ec) {  if (ec != boost::asio::error::operation_aborted) close(); });
					}
				}

				void writeexpire_from_now(int seconds)
				{
					return;//DONT USE THIS YET
					auto self(shared_from_this());
					_SocketImpl.StartWriteTimer(seconds);
					if (seconds >= 0) {
						_SocketImpl.get_write_deadline_timer().async_wait([this, self](boost::system::error_code ec) {  if (ec != boost::asio::error::operation_aborted) close(); });
					}
				}

		
			};
		};
	};
};