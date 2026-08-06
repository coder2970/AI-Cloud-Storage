cmake_minimum_required(VERSION 3.16)

project(AI_YunCunChu LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin_cgi)

include_directories(
    ${CMAKE_SOURCE_DIR}/include
    /usr/include/fastdfs
    /usr/include/fastcommon
    /usr/local/include/hiredis
    /usr/include/mysql
    /usr/include/jsoncpp
    /usr/local/include
)

link_directories(/usr/local/lib)

add_compile_options(
    -Wall
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-write-strings>
)

add_library(ycc_common STATIC
    common/cpp_cgi.cpp
    common/make_log.cpp
    common/cfg.cpp
    common/md5.c
)

add_library(ycc_ai STATIC
    common/dashscope_api.cpp
    common/faiss_wrapper.cpp
)

set(YCC_COMMON_LIBS
    fdfsclient
    fastcommon
    hiredis
    fcgi
    m
    mysqlclient
    jsoncpp
)

set(YCC_AI_LIBS
    faiss
    curl
    openblas
    gomp
    pthread
)

function(add_cgi_target target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE ycc_common ${YCC_COMMON_LIBS})
endfunction()

add_cgi_target(login src_cgi/login_cgi.cpp)
add_cgi_target(register src_cgi/reg_cgi.cpp)
add_cgi_target(upload src_cgi/upload_cgi.cpp)
add_cgi_target(md5 src_cgi/md5_cgi.cpp)
add_cgi_target(myfiles src_cgi/myfiles_cgi.cpp)
add_cgi_target(dealfile src_cgi/dealfile_cgi.cpp)
add_cgi_target(sharefiles src_cgi/sharefiles_cgi.cpp)
add_cgi_target(dealsharefile src_cgi/dealsharefile_cgi.cpp)
add_cgi_target(sharepicture src_cgi/sharepicture_cgi.cpp)
add_cgi_target(chunk_init src_cgi/chunk_init_cgi.cpp)
add_cgi_target(chunk_upload src_cgi/chunk_upload_cgi.cpp)
add_cgi_target(chunk_merge src_cgi/chunk_merge_cgi.cpp)

add_executable(ai src_cgi/ai_cgi.cpp)
target_link_libraries(ai PRIVATE ycc_ai ycc_common ${YCC_COMMON_LIBS} ${YCC_AI_LIBS})
