/*
	restinio
*/

/*!
	WebSocket connection routine.
*/

#pragma once

#include <asio.hpp>

#include <nodejs/http_parser/http_parser.h>

#include <fmt/format.h>

#include <restinio/all.hpp>
#include <restinio/websocket/message.hpp>
#include <restinio/websocket/impl/ws_parser.hpp>
#include <restinio/websocket/impl/utf8.hpp>

namespace restinio
{

namespace websocket
{

namespace impl
{

using ws_weak_handle_t = std::weak_ptr< ws_t >;

constexpr size_t WEBSOCKET_HEADER_MAX_SIZE = 14;

//
// ws_outgoing_data_t
//

//! A queue for outgoing buffers.
class ws_outgoing_data_t
{
	public:
		//! Add buffers to queue.
		void
		append( buffers_container_t bufs )
		{
			assert( !m_close_when_done );

			if( m_awaiting_buffers.empty() )
			{
				m_awaiting_buffers = std::move( bufs );
			}
			else
			{
				m_awaiting_buffers.reserve( m_awaiting_buffers.size() + bufs.size() );
				for( auto & buf : bufs )
					m_awaiting_buffers.emplace_back( std::move( buf ) );
			}
		}

		void
		pop_ready_buffers(
			std::size_t max_buf_count,
			buffers_container_t & bufs )
		{
			if( max_buf_count >= m_awaiting_buffers.size() )
				bufs = std::move( m_awaiting_buffers );
			else
			{
				const auto begin_of_bunch = m_awaiting_buffers.begin();
				const auto end_of_bunch = begin_of_bunch + max_buf_count;
				bufs.reserve( max_buf_count );
				for( auto it = begin_of_bunch; it != end_of_bunch; ++it )
				{
					bufs.emplace_back( std::move( *it ) );
				}

				m_awaiting_buffers.erase( begin_of_bunch, end_of_bunch );
			}
		}

	private:
		//! Flag is set when user initiates close.
		/*!
			If flag is switched on, then after sending all the buffers
			the socket mus be closed.
		*/
		bool m_close_when_done{ false };

		//! A queue of buffers.
		buffers_container_t m_awaiting_buffers;
};

//
// connection_input_t
//

//! Websocket input stuff.
struct connection_input_t
{
	connection_input_t( std::size_t buffer_size )
		:	m_buf{ buffer_size }
	{}

	//! websocket parser.
	ws_parser_t m_parser;

	//! Input buffer.
	restinio::impl::fixed_buffer_t m_buf;

	//! Current payload.
	std::string m_payload;

	//! Prepare parser for reading new http-message.
	void
	reset_parser_and_payload()
	{
		m_parser.reset();
		m_payload.clear();
	}
};

//! Check current websocket message header has correct flags and fields.
bool
validate_current_ws_message_header( const message_details_t & md )
{
	if( md.m_masking_key == 0 )
	{
		return false;
	}

	if( md.m_rsv1_flag != 0 ||
		md.m_rsv2_flag != 0 ||
		md.m_rsv3_flag != 0 )
	{
		return false;
	}

	return true;
}

//! Check current websocket message body is correct.
bool
validate_current_ws_message_body( const message_details_t & md, std::string & payload )
{
	if( md.m_mask_flag == true )
	{
		mask_unmask_payload(
			md.m_masking_key, payload );
	}

	if( md.m_opcode == opcode_t::text_frame)
	{
		return check_utf8_is_correct( payload );
	}

	return true;
}


//
// ws_connection_t
//

//! Context for handling websocket connections.
/*
*/
template <
		typename TRAITS,
		typename WS_MESSAGE_HANDLER >
class ws_connection_t final
	:	public ws_connection_base_t
{
	public:
		using message_handler_t = WS_MESSAGE_HANDLER;

		using timer_factory_t = typename TRAITS::timer_factory_t;
		using timer_guard_instance_t = typename timer_factory_t::timer_guard_instance_t;
		using logger_t = typename TRAITS::logger_t;
		using strand_t = typename TRAITS::strand_t;
		using stream_socket_t = typename TRAITS::stream_socket_t;

		ws_connection_t(
			//! Connection id.
			std::uint64_t conn_id,
			//! Data inherited from http-connection.
			//! \{
			stream_socket_t socket,
			strand_t strand,
			timer_guard_instance_t timer_guard,
			restinio::impl::connection_settings_shared_ptr_t< TRAITS > settings,
			//! \}
			message_handler_t msg_handler )
			:	ws_connection_base_t{ conn_id }
			,	m_socket{ std::move( socket ) }
			,	m_strand{ std::move( strand ) }
			,	m_timer_guard{ std::move( timer_guard ) }
			,	m_settings{ std::move( settings ) }
			,	m_input{ WEBSOCKET_HEADER_MAX_SIZE }
			,	m_msg_handler{ std::move( msg_handler ) }
			,	m_logger{ *( m_settings->m_logger ) }
		{
			// Notify of a new connection instance.
			m_logger.trace( [&]{
					return fmt::format(
						"[ws_connection:{}] start connection with {}",
						connection_id(),
						m_socket.remote_endpoint() );
			} );
		}

		ws_connection_t( const ws_connection_t & ) = delete;
		ws_connection_t( ws_connection_t && ) = delete;
		const ws_connection_t & operator = ( const ws_connection_t & ) = delete;
		ws_connection_t & operator = ( ws_connection_t && ) = delete;

		~ws_connection_t()
		{
			try
			{
				// Notify of a new connection instance.
				m_logger.trace( [&]{
					return fmt::format(
						"[ws_connection:{}] destroyed",
						connection_id() );
				} );
			}
			catch( ... )
			{}
		}

		//! Shutdown websocket.
		virtual void
		shutdown() override
		{
			asio::dispatch(
				get_executor(),
				[ this, ctx = shared_from_this() ](){
					try
					{
						m_logger.trace( [&]{
							return fmt::format(
								"[ws_connection:{}] shutdown",
								connection_id() );
						} );

						m_close_frame_to_user.disable();
						graceful_close();
					}
					catch( const std::exception & ex )
					{
						m_logger.error( [&]{
							return fmt::format(
								"[ws_connection:{}] shutdown operation error: {}",
								connection_id(),
								ex.what() );
						} );
					}
			} );
		}

		//! Kill websocket.
		virtual void
		kill() override
		{
			asio::dispatch(
				get_executor(),
				[ this, ctx = shared_from_this() ](){
					try
					{
						m_logger.trace( [&]{
							return fmt::format(
								"[ws_connection:{}] kill",
								connection_id() );
						} );

						m_close_frame_to_user.disable();
						m_close_frame_to_peer.disable();

						close_impl();
					}
					catch( const std::exception & ex )
					{
						m_logger.error( [&]{
							return fmt::format(
								"[ws_connection:{}] kill operation error: {}",
								connection_id(),
								ex.what() );
						} );
					}
			} );
		}

		//! Start reading ws-messages.
		void
		init_read( ws_handle_t wsh ) override
		{
			ws_weak_handle_t wswh{ wsh };
			//! Run write message on io_context loop if possible.
			asio::dispatch(
				get_executor(),
				[ this, ctx = shared_from_this(), wswh = std::move( wswh ) ](){
					try
					{
						m_websocket_weak_handle = std::move( wswh );
						m_read_state = read_state_t::read_any_frame;
						start_read_header();
					}
					catch( const std::exception & ex )
					{
						trigger_error_and_close(
							status_code_t::unexpected_condition,
							[&]{
								return fmt::format(
									"[ws_connection:{}] unable to init read: {}",
									connection_id(),
									ex.what() );
							} );
					}
			} );
		}

		//! Write pieces of outgoing data.
		virtual void
		write_data(
			buffers_container_t bufs,
			bool is_close_frame ) override
		{
			//! Run write message on io_context loop if possible.
			asio::dispatch(
				get_executor(),
				[ this,
					actual_bufs = std::move( bufs ),
					ctx = shared_from_this(),
					is_close_frame ]() mutable {
						try
						{
							if( write_state_t::write_enabled == m_write_state )
								write_data_impl(
									std::move( actual_bufs ),
									is_close_frame );
							else
							{
								m_logger.warn( [&]{
									return fmt::format(
											"[ws_connection:{}] cannot write to websocket: "
											"write operations disabled",
											connection_id() );
								} );
							}
						}
						catch( const std::exception & ex )
						{
							trigger_error_and_close(
								status_code_t::unexpected_condition,
								[&]{
									return fmt::format(
										"[ws_connection:{}] unable to write data: {}",
										connection_id(),
										ex.what() );
								} );
						}
				} );
		}
	private:
		//! An executor for callbacks on async operations.
		inline strand_t &
		get_executor()
		{
			return m_strand;
		}

		//! Standard close routine.
		void
		close_impl()
		{
			m_logger.trace( [&]{
				return fmt::format(
						"[ws_connection:{}] close socket",
						connection_id() );
			} );

			asio::error_code ignored_ec;
			m_socket.shutdown(
				asio::ip::tcp::socket::shutdown_both,
				ignored_ec );
			m_socket.close();
		}

		//! Start waiting for close-frame.
		void
		start_waiting_close_frame_only()
		{
			m_read_state = read_state_t::read_only_close_frame;

			// TODO: controll timeout.
		}

		//! Close WebSocket connection in a graceful manner.
		void
		graceful_close()
		{
			m_close_frame_to_peer.run_if_first(
				[&]{
					init_close_handshake( status_code_t::normal_closure );
					start_waiting_close_frame_only();
				} );
		}

		void
		init_close_handshake( std::string payload )
		{
			buffers_container_t bufs;
			bufs.reserve( 2 );

			bufs.emplace_back(
				impl::write_message_details(
					message_details_t{
						true,
						opcode_t::connection_close_frame,
						payload.size() } ) );

			bufs.emplace_back( std::move( payload ) );
			m_awaiting_buffers.append( std::move( bufs ) );

			init_write_if_necessary();

			m_write_state = write_state_t::write_disabled;
		}

		void
		init_close_handshake(
			status_code_t code,
			std::string desc = std::string{} )
		{
			init_close_handshake( std::string{ status_code_to_bin( code ) + desc } );
		}

		//! Trigger an error.
		/*!
			Writes error message to log,
			closes socket,
			and sends close frame to user if necessary.
		*/
		template< typename MSG_BUILDER >
		void
		trigger_error_and_close(
			status_code_t status,
			MSG_BUILDER && msg_builder )
		{
			m_logger.error( std::move( msg_builder ) );
			close_impl();
			m_close_frame_to_user.run_if_first(
				[&]{
					call_close_handler( status );
				} );
		}


		//! Start the process of reading ws messages from socket.
		void
		start_read_header()
		{
			m_logger.trace( [&]{
				return fmt::format(
						"[ws_connection:{}] start reading header",
						connection_id() );
			} );

			// Prepare parser for consuming new message.
			m_input.reset_parser_and_payload();

			if( 0 == m_input.m_buf.length() )
			{
				consume_header_from_socket();
			}
			else
			{
				// Has something to read from m_input.m_buf.
				consume_header_from_buffer(
					m_input.m_buf.bytes(), m_input.m_buf.length() );
			}
		}

		void
		consume_header_from_socket()
		{
			m_logger.trace( [&]{
				return fmt::format(
						"[ws_connection:{}] continue reading message",
						connection_id() );
			} );

			m_socket.async_read_some(
				m_input.m_buf.make_asio_buffer(),
				asio::bind_executor(
					get_executor(),
					[ this, ctx = shared_from_this() ](
						const asio::error_code & ec,
						std::size_t length ){
							try
							{
								after_read_header( ec, length );
							}
							catch( const std::exception & ex )
							{
								trigger_error_and_close(
									status_code_t::unexpected_condition,
									[&]{
										return fmt::format(
											"[ws_connection:{}] after read header callback error: {}",
											connection_id(),
											ex.what() );
									} );
							}
						} ) );
		}

		//! Handle read error (reading header or payload)
		void
		handle_read_error( const char * desc, const std::error_code & ec )
		{
			// Assume that connection is lost.
			trigger_error_and_close(
				status_code_t::connection_lost,
				[&]{
					return fmt::format(
						"[ws_connection:{}] {}: {}",
						connection_id(),
						desc,
						ec.message() );
				} );
		}

		//! Handle read operation result, when reading header.
		void
		after_read_header(
			const std::error_code & ec,
			std::size_t length )
		{
			if( !ec )
			{
				m_logger.trace( [&]{
					return fmt::format(
							"[ws_connection:{}] received {} bytes",
							this->connection_id(),
							length );
				} );

				m_input.m_buf.obtained_bytes( length );
				consume_header_from_buffer( m_input.m_buf.bytes(), length );
			}
			else
			{
				handle_read_error( "reading message header error", ec );
			}
		}

		//! Parse header from internal buffer.
		void
		consume_header_from_buffer( const char * data, std::size_t length )
		{
			const auto nparsed = m_input.m_parser.parser_execute( data, length );

			m_input.m_buf.consumed_bytes( nparsed );

			if( m_input.m_parser.header_parsed() )
			{
				auto & md = m_input.m_parser.current_message();

				if( !validate_current_ws_message_header( md ) )
				{
					m_logger.error( [&]{
						return fmt::format(
								"[ws_connection:{}] invalid header",
								connection_id() );
					} );

					if( read_state_t::read_any_frame == m_read_state )
					{
						m_close_frame_to_peer.run_if_first(
							[&]{
								init_close_handshake( status_code_t::protocol_error );
								// Do not wait anything in return, because
								// protocol is violated.
							} );

						m_close_frame_to_user.run_if_first(
							[&]{
								call_close_handler( status_code_t::protocol_error );
							} );

					}
					else if( read_state_t::read_only_close_frame == m_read_state )
					{
						// Wait for close frame cannot be done.
						close_impl();
					}

					return;
				}

				auto payload_length = md.payload_len();
				m_input.m_payload.resize( payload_length );

				if( payload_length == 0 )
				{
					// Callback for message with 0-size payload.
					call_handler_on_current_message();
				}
				else
				{
					const auto payload_part_size =
						std::min(
							m_input.m_buf.length(),
							payload_length );

					std::memcpy(
						&m_input.m_payload.front(),
						m_input.m_buf.bytes(),
						payload_part_size );

					m_input.m_buf.consumed_bytes( payload_part_size );

					if( payload_part_size == payload_length )
					{
						// All message is obtained.
						call_handler_on_current_message();
					}
					else
					{
						// Read the rest of payload:
						start_read_payload(
							&m_input.m_payload.front() + payload_part_size,
							payload_length - payload_part_size );
					}
				}
			}
			else
			{
				assert( nparsed == length );
				consume_header_from_socket();
			}
		}

		//! Start reading message payload.
		void
		start_read_payload(
			//! A pointer to the remainder of unfetched payload.
			char * payload_data,
			//! The size of the remainder of unfetched payload.
			std::size_t length_remaining )
		{
			m_socket.async_read_some(
				asio::buffer( payload_data, length_remaining ),
				asio::bind_executor(
					get_executor(),
					[ this,
						ctx = shared_from_this(),
						payload_data,
						length_remaining ](
						const asio::error_code & ec,
						std::size_t length ){

							try
							{
								after_read_payload(
									payload_data,
									length_remaining,
									ec,
									length );
							}
							catch( const std::exception & ex )
							{
								trigger_error_and_close(
									status_code_t::unexpected_condition,
									[&]{
										return fmt::format(
											"[ws_connection:{}] after read payload callback error: {}",
											connection_id(),
											ex.what() );
									} );
							}
						} ) );
		}

		//! Handle read operation result, when reading payload.
		void
		after_read_payload(
			char * payload_data,
			std::size_t length_remaining,
			const std::error_code & ec,
			std::size_t length )
		{
			if( !ec )
			{
				m_logger.trace( [&]{
					return fmt::format(
							"[ws_connection:{}] received {} bytes",
							this->connection_id(),
							length );
				} );

				if( length < length_remaining )
				{
					//Here: not all payload is obtained,
					// so inintiate read once again:
					this->start_read_payload(
						payload_data + length,
						length_remaining - length );
				}
				else
				{
					// Here: all the payload is ready.
					assert( length == length_remaining );

					// All message is obtained.
					call_handler_on_current_message();
				}
			}
			else
			{
				handle_read_error( "reading message payload error", ec );
			}
		}


		//! Call user message handler with current message.
		void
		call_message_handler( message_handle_t close_frame )
		{
			if( auto wsh = m_websocket_weak_handle.lock() )
			{
				try
				{
					m_msg_handler(
						std::move( wsh ),
						std::move( close_frame ) );
				}
				catch( const std::exception & ex )
				{
					m_logger.trace( [&]{
						return fmt::format(
								"[ws_connection:{}] execute handler error: {}",
								connection_id(),
								ex.what() );
					} );
				}
			}
		}

		void
		call_handler_on_current_message()
		{
			auto & md = m_input.m_parser.current_message();
			if( !validate_current_ws_message_body( md, m_input.m_payload ) )
			{
				m_logger.error( [&]{
					return fmt::format(
							"[ws_connection:{}] invalid paload",
							connection_id() );
				} );

				if( read_state_t::read_any_frame == m_read_state )
				{
					m_close_frame_to_peer.run_if_first(
						[&]{
							init_close_handshake( status_code_t::invalid_message_data );
							start_waiting_close_frame_only();
						} );

					m_close_frame_to_user.run_if_first(
						[&]{
							call_close_handler( status_code_t::invalid_message_data );
						} );
				}

				if( read_state_t::read_only_close_frame == m_read_state )
				{
					// Wait for next frame.
					start_read_header();
				}
			}
			else
			{
				if( read_state_t::read_any_frame == m_read_state )
				{
					if( opcode_t::connection_close_frame == md.m_opcode )
					{
						m_logger.trace( [&]{
							return fmt::format(
									"[ws_connection:{}] got close frame from peer, status: {}",
									connection_id(),
									(std::uint16_t)status_code_from_bin( m_input.m_payload ) );
						} );

						m_close_frame_to_user.disable();
						m_close_frame_to_peer.run_if_first(
							[&]{
								init_close_handshake( m_input.m_payload );
							} );

						m_read_state = read_state_t::read_nothing;
					}

					call_message_handler(
						std::make_shared< message_t >(
							md.m_final_flag,
							md.m_opcode,
							std::move( m_input.m_payload ) ) );

					if( read_state_t::read_nothing != m_read_state )
						start_read_header();
				}
				else
				{
					assert( read_state_t::read_only_close_frame == m_read_state );

					if( opcode_t::connection_close_frame == md.m_opcode )
					{
						// Got it!
						close_impl();

						m_logger.trace( [&]{
							return fmt::format(
									"[ws_connection:{}] expected close frame came",
									connection_id() );
						} );
					}
					else
					{
						// Wait for next frame.
						start_read_header();
					}
				}
			}
		}

		void
		call_close_handler( status_code_t status )
		{
			call_message_handler(
				std::make_shared< message_t >(
					true,
					opcode_t::connection_close_frame,
					status_code_to_bin( status ) ) );
		}

		//! Implementation of writing data performed on the asio::io_context.
		void
		write_data_impl( buffers_container_t bufs, bool is_close_frame )
		{
			if( !m_socket.is_open() )
			{
				m_logger.warn( [&]{
					return fmt::format(
							"[ws_connection:{}] try to write response, "
							"while socket is closed",
							connection_id() );
				} );
				return;
			}

			// Push buffers to queue.
			m_awaiting_buffers.append( std::move( bufs ) );

			if( is_close_frame )
			{
				m_close_frame_to_peer.disable();
				// No more writes.
				m_write_state = write_state_t::write_disabled;

				//TODO start waiting only close-frame.
				start_waiting_close_frame_only();
			}

			init_write_if_necessary();
		}

		//! Checks if there is something to write,
		//! and if so starts write operation.
		void
		init_write_if_necessary()
		{
			if( !m_resp_out_ctx.transmitting() )
			{
				// Here: not writing anything to socket, so
				// write operation can be initiated.
				if( m_resp_out_ctx.obtain_bufs( m_awaiting_buffers ) )
				{
					// Here: and there is smth to write.

					// Asio buffers (param for async write):
					auto & bufs = m_resp_out_ctx.create_bufs();

					m_logger.trace( [&]{
						return fmt::format(
							"[ws_connection:{}] sending data, "
							"buf count: {}",
							connection_id(),
							bufs.size() ); } );

					guard_write_operation();

					// There is somethig to write.
					asio::async_write(
						m_socket,
						bufs,
						asio::bind_executor(
							get_executor(),
							[ this,
								ctx = shared_from_this() ]
								( const asio::error_code & ec, std::size_t written ){
									try
									{
										after_write( ec, written );
									}
									catch( const std::exception & ex )
									{
										trigger_error_and_close(
											status_code_t::unexpected_condition,
											[&]{
												return fmt::format(
													"[ws_connection:{}] after write callback error: {}",
													connection_id(),
													ex.what() );
											} );
									}
							} ) );
				}
			}
		}

		//! Handle write response finished.
		inline void
		after_write(
			const std::error_code & ec,
			std::size_t written )
		{
			if( !ec )
			{
				// Release buffers.
				m_resp_out_ctx.done();

				m_logger.trace( [&]{
					return fmt::format(
							"[ws_connection:{}] outgoing data was sent: {}b",
							connection_id(),
							written );
				} );

				if( m_socket.is_open() )
				{
					// Start another write opertion
					// if there is something to send.
					init_write_if_necessary();
				}
			}
			else
			{
				trigger_error_and_close(
					status_code_t::connection_lost,
					[&]{
						return fmt::format(
							"[ws_connection:{}] unable to write: {}",
							connection_id(),
							ec.message() );
					} );
			}
		}

		//! Connection.
		stream_socket_t m_socket;

		//! Sync object for connection events.
		strand_t m_strand;

		//! Operation timeout guard.
		timer_guard_instance_t m_timer_guard;

		//! Start guard write operation if necessary.
		void
		guard_write_operation()
		{
			std::weak_ptr< ws_connection_base_t > weak_ctx = shared_from_this();

			m_timer_guard
				->schedule_operation_timeout_callback(
					get_executor(),
					m_settings->m_write_http_response_timelimit,
					[ this, weak_ctx = std::move( weak_ctx ) ](){
						if( auto ctx = weak_ctx.lock() )
						{
							m_logger.trace( [&]{
								return fmt::format(
										"[wd_connection:{}] write operation timed out",
										this->connection_id() );
								} );
							m_close_frame_to_peer.disable();
							m_close_frame_to_user.run_if_first(
								[&]{
									call_close_handler( status_code_t::unexpected_condition );
								} );
							close_impl();
						}
					} );
		}

		//! Common paramaters of a connection.
		restinio::impl::connection_settings_shared_ptr_t< TRAITS > m_settings;

		//! Logger for operation
		logger_t & m_logger;

		//! Input routine.
		connection_input_t m_input;

		message_handler_t m_msg_handler;

		//! Write to socket operation context.
		restinio::impl::raw_resp_output_ctx_t m_resp_out_ctx;

		//! Output buffers queue.
		ws_outgoing_data_t m_awaiting_buffers;

		//! A waek handler for owning ws_t to use it when call message handler.
		ws_weak_handle_t m_websocket_weak_handle;

		//! Websocket output states.
		enum class write_state_t
		{
			//! Able to append outgoing data.
			write_enabled,
			//! No more outgoing data can be added (e.g. close-frame was sent).
			write_disabled
		};

		//! A state of a websocket output.
		write_state_t m_write_state = write_state_t::write_enabled;

		//! Websocket input states.
		enum class read_state_t
		{
			//! Reads any type of frame and serve it to user.
			read_any_frame,
			//! Reads only close frame: skip all frames until close-frame.
			read_only_close_frame,
			//! Do not read anything (before activation).
			read_nothing
		};

		//! A state of a websocket input.
		read_state_t m_read_state = read_state_t::read_nothing;


		//! A helper class for running exclusive action.
		//! Only a first action will run.
		class one_shot_action_t
		{
			public:
				template < typename Action >
				void
				run_if_first( Action && action )
				{
					if( m_not_executed_yet )
					{
						m_not_executed_yet = false;
						action();
					}
				}

				//! Disable ation: action will not be executed even on a first shot.
				void
				disable()
				{
					m_not_executed_yet = false;
				}

			private:
				bool m_not_executed_yet{ true };
		};

		one_shot_action_t m_close_frame_to_user;
		one_shot_action_t m_close_frame_to_peer;
};

// //! Context for handling websocket connections.
// /*
// */
// template <
// 		typename TRAITS,
// 		typename WS_MESSAGE_HANDLER >
// class ws_connection_t final
// 	:	public ws_connection_base_t
// {
// 	public:
// 		using message_handler_t = WS_MESSAGE_HANDLER;

// 		using timer_factory_t = typename TRAITS::timer_factory_t;
// 		using timer_guard_instance_t = typename timer_factory_t::timer_guard_instance_t;
// 		using logger_t = typename TRAITS::logger_t;
// 		using strand_t = typename TRAITS::strand_t;
// 		using stream_socket_t = typename TRAITS::stream_socket_t;

// 		ws_connection_t(
// 			//! Connection id.
// 			std::uint64_t conn_id,
// 			//! Connection socket.
// 			stream_socket_t socket,
// 			strand_t strand,
// 			timer_guard_instance_t timer_guard,
// 			//! Settings that are common for connections.
// 			restinio::impl::connection_settings_shared_ptr_t< TRAITS > settings,
// 			message_handler_t msg_handler )
// 			:	ws_connection_base_t{ conn_id }
// 			,	m_socket{ std::move( socket ) }
// 			,	m_strand{ std::move( strand ) }
// 			,	m_settings{ std::move( settings ) }
// 			,	m_timer_guard{ std::move( timer_guard ) }
// 			,	m_input{ WEBSOCKET_HEADER_MAX_SIZE }
// 			,	m_msg_handler{ std::move( msg_handler ) }
// 			,	m_logger{ *( m_settings->m_logger ) }
// 		{
// 			// Notify of a new connection instance.
// 			m_logger.trace( [&]{
// 					return fmt::format(
// 						"[ws_connection:{}] start connection with {}",
// 						connection_id(),
// 						m_socket.remote_endpoint() );
// 			} );
// 		}

// 		ws_connection_t( const ws_connection_t & ) = delete;
// 		ws_connection_t( ws_connection_t && ) = delete;
// 		const ws_connection_t & operator = ( const ws_connection_t & ) = delete;
// 		ws_connection_t & operator = ( ws_connection_t && ) = delete;

// 		~ws_connection_t()
// 		{
// 			try
// 			{
// 				// Notify of a new connection instance.
// 				m_logger.trace( [&]{
// 					return fmt::format(
// 						"[ws_connection:{}] destroyed",
// 						connection_id() );
// 				} );
// 			}
// 			catch( ... )
// 			{}
// 		}

// 		//! Shutdown websocket.
// 		virtual void
// 		shutdown() override
// 		{
// 			asio::dispatch(
// 				get_executor(),
// 				[ this, ctx = shared_from_this() ](){
// 					try
// 					{
// 						m_logger.trace( [&]{
// 							return fmt::format(
// 								"[ws_connection:{}] shutdown",
// 								connection_id() );
// 						} );
// 						graceful_close();
// 					}
// 					catch( const std::exception & ex )
// 					{
// 						m_logger.error( [&]{
// 							return fmt::format(
// 								"[ws_connection:{}] shutdown operation error: {}",
// 								connection_id(),
// 								ex.what() );
// 						} );
// 					}
// 			} );
// 		}

// 		//! Kill websocket.
// 		virtual void
// 		kill() override
// 		{
// 			asio::dispatch(
// 				get_executor(),
// 				[ this, ctx = shared_from_this() ](){
// 					try
// 					{
// 						m_logger.trace( [&]{
// 							return fmt::format(
// 								"[ws_connection:{}] kill",
// 								connection_id() );
// 						} );

// 						close_impl();
// 						// TODO: do not notify user?
// 					}
// 					catch( const std::exception & ex )
// 					{
// 						m_logger.error( [&]{
// 							return fmt::format(
// 								"[ws_connection:{}] kill operation error: {}",
// 								connection_id(),
// 								ex.what() );
// 						} );
// 					}
// 			} );
// 		}

// 		//! Start reading ws-messages.
// 		void
// 		init_read( ws_handle_t wsh ) override
// 		{
// 			ws_weak_handle_t wswh{ wsh };
// 			//! Run write message on io_context loop if possible.
// 			asio::dispatch(
// 				get_executor(),
// 				[ this, ctx = shared_from_this(), wswh = std::move( wswh ) ](){
// 					try
// 					{
// 						m_websocket_weak_handle = std::move( wswh );
// 						start_read_header();
// 					}
// 					catch( const std::exception & ex )
// 					{
// 						trigger_error_and_close(
// 							ex.what(),
// 							[&]{
// 								return fmt::format(
// 									"[ws_connection:{}] unable to init read: {}",
// 									connection_id(),
// 									ex.what() );
// 							} );
// 					}
// 			} );
// 		}

// 		//! Write pieces of outgoing data.
// 		virtual void
// 		write_data(
// 			buffers_container_t bufs,
// 			bool is_close_frame ) override
// 		{
// 			//! Run write message on io_context loop if possible.
// 			asio::dispatch(
// 				get_executor(),
// 				[ this,
// 					actual_bufs = std::move( bufs ),
// 					ctx = shared_from_this(),
// 					is_close_frame ]() mutable {
// 						try
// 						{
// 							if( write_state_t::write_enabled == m_write_state )
// 								write_data_impl(
// 									std::move( actual_bufs ),
// 									is_close_frame );
// 							else
// 							{
// 								m_logger.warn( [&]{
// 									return fmt::format(
// 											"[ws_connection:{}] cannot write to websocket: "
// 											"write operations disabled",
// 											connection_id() );
// 								} );
// 							}
// 						}
// 						catch( const std::exception & ex )
// 						{
// 							trigger_error_and_close(
// 								ex.what(),
// 								[&]{
// 									return fmt::format(
// 										"[ws_connection:{}] unable to write data: {}",
// 										connection_id(),
// 										ex.what() );
// 								} );
// 						}
// 				} );
// 		}

// 	private:
// 		//! Start the process of reading ws messages from socket.
// 		void
// 		start_read_header()
// 		{
// 			m_logger.trace( [&]{
// 				return fmt::format(
// 						"[ws_connection:{}] start reading header",
// 						connection_id() );
// 			} );

// 			// Prepare parser for consuming new message.
// 			m_input.reset_parser_and_payload();

// 			if( 0 == m_input.m_buf.length() )
// 			{
// 				consume_message();
// 			}
// 			else
// 			{
// 				// Has something to read from m_input.m_buf.
// 				consume_header_from_buffer(
// 					m_input.m_buf.bytes(), m_input.m_buf.length() );
// 			}
// 		}

// 		inline void
// 		consume_message()
// 		{
// 			m_logger.trace( [&]{
// 				return fmt::format(
// 						"[ws_connection:{}] continue reading message",
// 						connection_id() );
// 			} );

// 			m_socket.async_read_some(
// 				m_input.m_buf.make_asio_buffer(),
// 				asio::bind_executor(
// 					get_executor(),
// 					[ this, ctx = shared_from_this() ](
// 						const asio::error_code & ec,
// 						std::size_t length ){
// 							try
// 							{
// 								after_read_header( ec, length );
// 							}
// 							catch( const std::exception & ex )
// 							{
// 								trigger_error_and_close(
// 									status_code_t::unexpected_condition,
// 									[&]{
// 										return fmt::format(
// 											"[ws_connection:{}] after read header callback error: {}",
// 											connection_id(),
// 											ex.what() );
// 									} );
// 							}
// 						} ) );
// 		}

// 		//! Handle read error (reading header or payload)
// 		void
// 		handle_read_error( const char * desc, const std::error_code & ec )
// 		{
// 			trigger_error_and_close(
// 				status_code_t::connection_lost,
// 				[&]{
// 					return fmt::format(
// 						"[ws_connection:{}] {}: {}",
// 						connection_id(),
// 						desc,
// 						ec.message() );
// 				} );
// 		}

// 		//! Handle read operation result, when reading header.
// 		void
// 		after_read_header(
// 			const std::error_code & ec,
// 			std::size_t length )
// 		{
// 			if( !ec )
// 			{
// 				m_input.m_buf.obtained_bytes( length );
// 				consume_header_from_buffer( m_input.m_buf.bytes(), length );
// 			}
// 			else
// 			{
// 				handle_read_error( ec );
// 			}
// 		}

// 		//! Parse header from internal buffer.
// 		void
// 		consume_header_from_buffer( const char * data, std::size_t length )
// 		{
// 			const auto nparsed = m_input.m_parser.parser_execute( data, length );

// 			m_input.m_buf.consumed_bytes( nparsed );

// 			if( m_input.m_parser.header_parsed() )
// 			{
// 				if( !validate_current_ws_message_header() )
// 				{
// 					// close_impl();
// 					// call_close_handler( "message validation failed" );

// 					init_close_handshake( status_code_t::protocol_error );
// 					return;
// 				}

// 				auto payload_length = m_input.m_parser.current_message().payload_len();
// 				m_input.m_payload.resize( payload_length );

// 				if( payload_length == 0 )
// 				{
// 					// Callback for message with 0-size payload.
// 					call_handler_on_current_message();
// 					return;
// 				}

// 				const auto payload_part_size =
// 					std::min(
// 						m_input.m_buf.length(),
// 						payload_length );

// 				std::memcpy(
// 					&m_input.m_payload.front(),
// 					m_input.m_buf.bytes(),
// 					payload_part_size );

// 				m_input.m_buf.consumed_bytes( payload_part_size );

// 				if( payload_part_size == payload_length )
// 				{
// 					// All message is obtained.
// 					call_handler_on_current_message();
// 				}
// 				else
// 				{
// 					// Read the rest of payload:
// 					start_read_payload(
// 						&m_input.m_payload.front() + payload_part_size,
// 						payload_length - payload_part_size );
// 				}
// 			}
// 			else
// 			{
// 				consume_message();
// 			}
// 		}

// 		//! Start reading message payload.
// 		void
// 		start_read_payload(
// 			//! A pointer to the remainder of unfetched payload.
// 			char * payload_data,
// 			//! The size of the remainder of unfetched payload.
// 			std::size_t length_remaining )
// 		{
// 			m_socket.async_read_some(
// 				asio::buffer( payload_data, length_remaining ),
// 				asio::bind_executor(
// 					get_executor(),
// 					[ this,
// 						ctx = shared_from_this(),
// 						payload_data,
// 						length_remaining ](
// 						const asio::error_code & ec,
// 						std::size_t length ){

// 							try
// 							{
// 								after_read_payload(
// 									payload_data,
// 									length_remaining,
// 									ec,
// 									length );
// 							}
// 							catch( const std::exception & ex )
// 							{
// 								trigger_error_and_close(
// 									ex.what(),
// 									[&]{
// 										return fmt::format(
// 											"[ws_connection:{}] after read payload callback error: {}",
// 											connection_id(),
// 											ex.what() );
// 									} );
// 							}
// 						} ) );
// 		}

// 		//! Handle read operation result, when reading payload.
// 		void
// 		after_read_payload(
// 			char * payload_data,
// 			std::size_t length_remaining,
// 			const std::error_code & ec,
// 			std::size_t length )
// 		{
// 			if( !ec )
// 			{
// 				if( length < length_remaining )
// 				{
// 					//Here: not all payload is obtained,
// 					// so inintiate read once again:
// 					this->start_read_payload(
// 						payload_data + length,
// 						length_remaining - length );
// 				}
// 				else
// 				{
// 					// Here: all the payload is ready.
// 					assert( length == length_remaining );

// 					// All message is obtained.
// 					call_handler_on_current_message();
// 				}
// 			}
// 			else
// 			{
// 				handle_read_error( ec );
// 			}
// 		}

// 		//! Implementation of writing data performed on the asio::io_context.
// 		void
// 		write_data_impl( buffers_container_t bufs, bool is_close_frame )
// 		{
// 			if( !m_socket.is_open() )
// 			{
// 				m_logger.warn( [&]{
// 					return fmt::format(
// 							"[ws_connection:{}] try to write response, "
// 							"while socket is closed",
// 							connection_id() );
// 				} );
// 				return;
// 			}

// 			// Push buffers to queue.
// 			m_awaiting_buffers.append( std::move( bufs ) );

// 			if( is_close_frame )
// 			{
// 				// No more writes.
// 				m_write_state = write_state_t::write_disabled;

// 				//TODO start waiting only close-frame.
// 				start_waiting_close_frame_only();
// 			}

// 			init_write_if_necessary();
// 		}

// 		//! Checks if there is something to write,
// 		//! and if so starts write operation.
// 		void
// 		init_write_if_necessary()
// 		{
// 			if( !m_resp_out_ctx.transmitting() )
// 			{
// 				// Here: not writing anything to socket, so
// 				// write operation can be initiated.
// 				if( m_resp_out_ctx.obtain_bufs( m_awaiting_buffers ) )
// 				{
// 					// Here: and there is smth to write.

// 					// Asio buffers (param for async write):
// 					auto & bufs = m_resp_out_ctx.create_bufs();

// 					m_logger.trace( [&]{
// 						return fmt::format(
// 							"[ws_connection:{}] sending data, "
// 							"buf count: {}",
// 							connection_id(),
// 							bufs.size() ); } );

// 					guard_write_operation();

// 					// There is somethig to write.
// 					asio::async_write(
// 						m_socket,
// 						bufs,
// 						asio::bind_executor(
// 							get_executor(),
// 							[ this,
// 								ctx = shared_from_this() ]
// 								( const asio::error_code & ec, std::size_t written ){
// 									try
// 									{
// 										after_write( ec, written );
// 									}
// 									catch( const std::exception & ex )
// 									{
// 										trigger_error_and_close(
// 											ex.what(),
// 											[&]{
// 												return fmt::format(
// 													"[ws_connection:{}] after write callback error: {}",
// 													connection_id(),
// 													ex.what() );
// 											} );
// 									}
// 							} ) );
// 				}
// 				// else if ( m_awaiting_buffers.close_when_done() )
// 				// {
// 				// 	close_impl();
// 				// 	call_close_handler( "user initiated shutdown" );
// 				// }
// 			}
// 		}

// 		//! Handle write response finished.
// 		inline void
// 		after_write(
// 			const std::error_code & ec,
// 			std::size_t written )
// 		{
// 			if( !ec )
// 			{
// 				// Release buffers.
// 				m_resp_out_ctx.done();

// 				m_logger.trace( [&]{
// 					return fmt::format(
// 							"[ws_connection:{}] outgoing data was sent: {}b",
// 							connection_id(),
// 							written );
// 				} );

// 				if( m_socket.is_open() )
// 				{
// 					// Start another write opertion
// 					// if there is something to send.
// 					init_write_if_necessary();
// 				}
// 			}
// 			else
// 			{
// 				trigger_error_and_close(
// 					ec.message(),
// 					[&]{
// 						return fmt::format(
// 							"[ws_connection:{}] unable to write: {}",
// 							connection_id(),
// 							ec.message() );
// 					} );
// 			}
// 		}

// 		//! Start waiting for close-frame.
// 		void
// 		start_waiting_close_frame_only()
// 		{
// 			m_read_state = read_state_t::read_only_close_frame;

// 			// TODO: controll timeout.
// 		}

// 		//! Close WebSocket connection in a graceful manner.
// 		void
// 		graceful_close()
// 		{
// 			if( write_state_t::write_enabled == m_write_state )
// 			{
// 				init_close_handshake( status_code_t::normal_closure );
// 				start_waiting_close_frame_only();
// 			}
// 		}

// 		//! An executor for callbacks on async operations.
// 		inline strand_t &
// 		get_executor()
// 		{
// 			return m_strand;
// 		}

// 		//! Close connection functions.
// 		//! \{

// 		//! Standard close routine.
// 		void
// 		close_impl()
// 		{
// 			m_logger.trace( [&]{
// 				return fmt::format(
// 						"[ws_connection:{}] close socket",
// 						connection_id() );
// 			} );

// 			asio::error_code ignored_ec;
// 			m_socket.shutdown(
// 				asio::ip::tcp::socket::shutdown_both,
// 				ignored_ec );
// 			m_socket.close();
// 		}

// 		//! Trigger an error.
// 		/*!
// 			Closes the connection and write to log
// 			an error message.
// 		*/
// 		template< typename MSG_BUILDER >
// 		void
// 		trigger_error_and_close(
// 			status_code_t status,
// 			MSG_BUILDER && msg_builder )
// 		{
// 			m_logger.error( std::move( msg_builder ) );
// 			close_impl();

// 			call_close_handler( status );
// 		}
// 		//! \}

// 		//! Call user message handler with current message.
// 		void
// 		call_message_handler( message_handle_t close_frame )
// 		{
// 			if( auto wsh = m_websocket_weak_handle.lock() )
// 			{
// 				m_msg_handler(
// 					std::move( wsh ),
// 					std::move( close_frame ) );
// 			}
// 		}

// 		void
// 		call_handler_on_current_message()
// 		{
// 			if( !validate_current_ws_message_body() )
// 			{
// 				if( write_state_t::write_enabled == m_write_state )
// 				{
// 					init_close_handshake( status_code_t::invalid_message_data );
// 					call_close_handler( status_code_t::invalid_message_data );
// 					start_waiting_close_frame_only();
// 				}
// 			}
// 			else
// 			{
// 				if( read_state_t::read_any_frame == m_read_state )
// 				{
// 					const auto & current_header = m_input.m_parser.current_message();

// 					if( opcode_t::connection_close_frame == m_input.m_parser.m_opcode )
// 					{
// 						m_logger.trace( [&]{
// 							return fmt::format(
// 									"[ws_connection:{}] got close frame from peer, status: {}",
// 									connection_id(),
// 									status_code_from_bin( m_input.m_payload ) );
// 						} );

// 						init_close_handshake( m_input.m_payload );
// 						m_read_state = read_state_t::read_nothing;

// 					}

// 					call_message_handler(
// 						std::make_shared< message_t >(
// 							m_input.m_parser.m_final_flag,
// 							m_input.m_parser.m_opcode,
// 							std::move( m_input.m_payload ) ) );

// 					if( read_state_t::read_nothing != m_read_state )
// 						start_read_header();
// 				}
// 				else
// 				{
// 					assert( read_state_t::read_only_close_frame == m_read_state );

// 					if( opcode_t::connection_close_frame == m_input.m_parser.m_opcode )
// 					{
// 						// Wait for next frame.
// 						close_impl();

// 						m_logger.trace( [&]{
// 							return fmt::format(
// 									"[ws_connection:{}] close frame came",
// 									connection_id() );
// 						} );
// 					}
// 					else
// 					{
// 						// Wait for next frame.
// 						start_read_header();
// 					}
// 				}
// 			}
// 		}

// 		void
// 		call_close_handler( status_code_t status )
// 		{
// 			call_message_handler(
// 				std::make_shared< message_t >(
// 					true,
// 					opcode_t::connection_close_frame,
// 					status_code_to_bin( status ) ) );
// 		}

// 		//! Start guard write operation if necessary.
// 		void
// 		guard_write_operation()
// 		{
// 			std::weak_ptr< ws_connection_base_t > weak_ctx = shared_from_this();

// 			m_timer_guard
// 				->schedule_operation_timeout_callback(
// 					get_executor(),
// 					m_settings->m_write_http_response_timelimit,
// 					[ this, weak_ctx = std::move( weak_ctx ) ](){
// 						if( auto ctx = weak_ctx.lock() )
// 						{
// 							m_logger.trace( [&]{
// 								return fmt::format(
// 										"[wd_connection:{}] write operation timed out",
// 										this->connection_id() );
// 								} );
// 							close_impl();
// 							call_close_handler( "write timeout" );
// 						}
// 					} );
// 		}

// 		//! Check current websocket message header has correct flags and fields.
// 		bool
// 		validate_current_ws_message_header() const
// 		{
// 			const auto & current_header = m_input.m_parser.current_message();

// 			if( current_header.m_masking_key == 0 )
// 			{
// 				return false;
// 			}

// 			if( current_header.m_rsv1_flag != 0 ||
// 				current_header.m_rsv2_flag != 0 ||
// 				current_header.m_rsv3_flag != 0 )
// 			{
// 				return false;
// 			}

// 			return true;
// 		}

// 		//! Check current websocket message body is correct.
// 		bool
// 		validate_current_ws_message_body()
// 		{
// 			const auto & current_header = m_input.m_parser.current_message();
// 			auto & current_payload = m_input.m_payload;

// 			if( current_header.m_mask_flag == true )
// 			{
// 				impl::mask_unmask_payload(
// 					current_header.m_masking_key, current_payload );
// 			}

// 			if( current_header.m_opcode == opcode_t::text_frame)
// 			{
// 				return check_utf8_is_correct( current_payload );
// 			}

// 			return true;
// 		}

// 		void
// 		init_close_handshake( std::string payload )
// 		{
// 			if( !m_close_was_sent_to_user )
// 			{
// 				const restinio::websocket::message_t
// 					close_msg{
// 						true,
// 						opcode_t::connection_close_frame,
// 						std::move( payload ) };

// 				buffers_container_t bufs;
// 				bufs.reserve( 2 );

// 				bufs.emplace_back(
// 					impl::write_message_details(
// 						message_details_t{ close_msg } ) );

// 				bufs.emplace_back( std::move( close_msg.payload() ) );
// 				m_awaiting_buffers.append( std::move( bufs ) );

// 				init_write_if_necessary();
// 			}

// 			m_close_was_sent_to_user = true;
// 			m_write_state = write_disabled;
// 		}

// 		void
// 		init_close_handshake(
// 			status_code_t code,
// 			std::string desc = std::string{} )
// 		{
// 			init_close_handshake( std::string{ status_code_to_bin( code ) + desc } );
// 		}

// 		//! Connection.
// 		stream_socket_t m_socket;

// 		//! Sync object for connection events.
// 		strand_t m_strand;

// 		//! Common paramaters of a connection.
// 		restinio::impl::connection_settings_shared_ptr_t< TRAITS > m_settings;

// 		//! Operation timeout guard.
// 		timer_guard_instance_t m_timer_guard;

// 		//! Input routine.
// 		connection_input_t m_input;

// 		message_handler_t m_msg_handler;

// 		ws_weak_handle_t m_websocket_weak_handle;
// 		//! Write to socket operation context.
// 		restinio::impl::raw_resp_output_ctx_t m_resp_out_ctx;

// 		//! Output buffers queue.
// 		ws_outgoing_data_t m_awaiting_buffers;

// 		//! Logger for operation
// 		logger_t & m_logger;

// 		//! Websocket output states.
// 		enum class write_state_t
// 		{
// 			//! Able to append outgoing data.
// 			write_enabled,
// 			//! No more outgoing data can be added (e.g. close-frame was sent).
// 			write_disabled
// 		};

// 		//! A state of a websocket output.
// 		write_state_t m_write_state = write_state_t::write_enabled;

// 		//! Websocket input states.
// 		enum class read_state_t
// 		{
// 			//! Reads any type of frame and serve it to user.
// 			read_any_frame,
// 			//! Reads only close frame: skip all frames until close-frame.
// 			read_only_close_frame,
// 			//! Do not read anything (before activation).
// 			read_nothing
// 		};

// 		//! A state of a websocket input.
// 		read_state_t m_read_state = read_state_t::read_nothing;

// 		bool m_close_was_sent_to_user{ false };
// 		bool m_close_was_sent_to_peer{ false };
// };

} /* namespace impl */

} /* namespace websocket */

} /* namespace restinio */
