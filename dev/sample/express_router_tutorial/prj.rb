require 'mxx_ru/cpp'

MxxRu::Cpp::exe_target {

  target 'sample.express_router_tutorial'

  required_prj 'asio_mxxru/prj.rb'
  required_prj 'nodejs/http_parser_mxxru/prj.rb'
  required_prj 'fmt_mxxru/prj.rb'
  required_prj 'sample/platform_specific_libs.rb'

  cpp_source 'main.cpp'
}