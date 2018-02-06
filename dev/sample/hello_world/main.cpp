#include <iostream>

#include <restinio/all.hpp>

#include <fmt/format.h>
#include <fmt/ostream.h>
#define BOOST_THREAD_VERSION 4
#define BOOST_THREAD_PROVIDES_EXECUTORS
#include <boost/thread/future.hpp>
#include <boost/thread/executors/basic_thread_pool.hpp>
#include <fstream>

template < typename RESP >
RESP
init_resp( RESP resp )
{
	resp.append_header( restinio::http_field::server, "RESTinio sample server /v.0.2" );
	resp.append_header_date_field();

	return resp;
};

using router_t = restinio::router::express_router_t<>;
boost::basic_thread_pool pool{std::thread::hardware_concurrency()};

auto create_request_handler()
{
	auto router = std::make_unique< router_t >();

    router->http_get(
            "/future",
            [](auto req, auto) {
                boost::future<std::string> f1 = boost::async(pool, []() {
					std::ifstream is("index.html", std::ios::in | std::ios::binary);
					std::string result;
					char buf[512];
					while (is.read(buf, sizeof(buf)).gcount() > 0)
						result.append(buf, is.gcount());
					return result;
                });
                f1.then(pool, [req](auto f) {
                    auto r = f.get();
                    init_resp( req->create_response() )
                            .append_header( restinio::http_field::content_type, "text/html; charset=utf-8" )
                            .set_body(r)
                            .done();
                });

                return restinio::request_accepted();
            }
    );
	router->http_get(
		"/",
		[]( auto req, auto ){
				init_resp( req->create_response() )
					.append_header( restinio::http_field::content_type, "text/plain; charset=utf-8" )
					.set_body( "Hello world!")
					.done();

				return restinio::request_accepted();
		} );

	router->http_get(
		"/json",
		[]( auto req, auto ){
				init_resp( req->create_response() )
					.append_header( restinio::http_field::content_type, "text/json; charset=utf-8" )
					.set_body( R"-({"message" : "Hello world!"})-")
					.done();

				return restinio::request_accepted();
		} );

	router->http_get(
		"/html",
		[]( auto req, auto ){
				init_resp( req->create_response() )
						.append_header( restinio::http_field::content_type, "text/html; charset=utf-8" )
						.set_body(
							"<html>\r\n"
							"  <head><title>Hello from RESTinio!</title></head>\r\n"
							"  <body>\r\n"
							"    <center><h1>Hello world</h1></center>\r\n"
							"  </body>\r\n"
							"</html>\r\n" )
						.done();

				return restinio::request_accepted();
		} );

	return router;
}

int main()
{
	using namespace std::chrono;

	try
	{
		using traits_t =
			restinio::traits_t<
				restinio::asio_timer_manager_t,
				restinio::null_logger_t,
				router_t >;

		restinio::run(
			restinio::on_thread_pool<traits_t>( std::thread::hardware_concurrency() )
				.port( 8080 )
				.address( "localhost" )
				.request_handler( create_request_handler() ) );
	}
	catch( const std::exception & ex )
	{
		std::cerr << "Error: " << ex.what() << std::endl;
		return 1;
	}

	return 0;
}
