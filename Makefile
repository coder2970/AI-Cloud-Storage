# Compiler
CC=gcc
CXX=g++

# Preprocess include paths
CPPLFAGS=-I./include \
		 -I/usr/include/fastdfs \
		 -I/usr/include/fastcommon \
		 -I/usr/local/include/hiredis/ \
		 -I/usr/include/mysql/ \
		 -I/usr/include/jsoncpp \
		 -I/usr/local/include

# Options
CFLAGS=-Wall
CXXFLAGS=-Wall -std=c++11 -Wno-write-strings

# Dynamic libraries
LIBS=-lfdfsclient \
	 -lfastcommon \
	 -lhiredis \
	 -lfcgi \
	 -lm \
	 -lmysqlclient \
	 -ljsoncpp

# AI extra libraries
AI_LIBS=-lfaiss -lcurl -lopenblas -lgomp -lpthread -L/usr/local/lib

# Directories
TEST_PATH=test
COMMON_PATH=common
CGI_BIN_PATH=bin_cgi
CGI_SRC_PATH=src_cgi

# Test targets
main=main
redis=redis

# Project targets
login=$(CGI_BIN_PATH)/login
register=$(CGI_BIN_PATH)/register
upload=$(CGI_BIN_PATH)/upload
md5=$(CGI_BIN_PATH)/md5
myfiles=$(CGI_BIN_PATH)/myfiles
dealfile=$(CGI_BIN_PATH)/dealfile
sharefiles=$(CGI_BIN_PATH)/sharefiles
dealsharefile=$(CGI_BIN_PATH)/dealsharefile
sharepicture=$(CGI_BIN_PATH)/sharepicture
chunk_init=$(CGI_BIN_PATH)/chunk_init
chunk_upload=$(CGI_BIN_PATH)/chunk_upload
chunk_merge=$(CGI_BIN_PATH)/chunk_merge
ai=$(CGI_BIN_PATH)/ai

# Final targets
target=$(login) \
	   $(register) \
	   $(upload) \
	   $(md5) \
	   $(myfiles) \
	   $(dealfile) \
	   $(sharefiles) \
	   $(dealsharefile) \
	   $(sharepicture) \
	   $(chunk_init) \
	   $(chunk_upload) \
	   $(chunk_merge) \
	   $(ai)

ALL:$(target)

#######################################################################
# Test programs
$(main):$(TEST_PATH)/main.o $(TEST_PATH)/fdfs_api.o $(COMMON_PATH)/make_log.o
	$(CC) $^ $(LIBS) -o $@

$(redis):$(TEST_PATH)/myredis.o
	$(CC) $^ $(LIBS) -o $@

#######################################################################
# Project programs
$(login):	$(CGI_SRC_PATH)/login_cgi.o \
			$(COMMON_PATH)/cpp_cgi.o \
			$(COMMON_PATH)/make_log.o \
			$(COMMON_PATH)/cfg.o \
			$(COMMON_PATH)/md5.o
	$(CXX) $^ -o $@ $(LIBS)

$(register):	$(CGI_SRC_PATH)/reg_cgi.o \
			$(COMMON_PATH)/cpp_cgi.o \
			$(COMMON_PATH)/make_log.o \
			$(COMMON_PATH)/cfg.o \
			$(COMMON_PATH)/md5.o
	$(CXX) $^ -o $@ $(LIBS)

$(md5):		$(CGI_SRC_PATH)/md5_cgi.o \
			$(COMMON_PATH)/cpp_cgi.o \
			$(COMMON_PATH)/make_log.o \
			$(COMMON_PATH)/cfg.o \
			$(COMMON_PATH)/md5.o
	$(CXX) $^ -o $@ $(LIBS)

$(upload):$(CGI_SRC_PATH)/upload_cgi.o \
		  $(COMMON_PATH)/cpp_cgi.o \
		  $(COMMON_PATH)/make_log.o \
		  $(COMMON_PATH)/cfg.o \
		  $(COMMON_PATH)/md5.o
	$(CXX) $^ -o $@ $(LIBS)

$(myfiles):	$(CGI_SRC_PATH)/myfiles_cgi.o \
			$(COMMON_PATH)/cpp_cgi.o \
			$(COMMON_PATH)/make_log.o \
			$(COMMON_PATH)/cfg.o \
			$(COMMON_PATH)/md5.o
	$(CXX) $^ -o $@ $(LIBS)

$(dealfile):$(CGI_SRC_PATH)/dealfile_cgi.o \
			$(COMMON_PATH)/cpp_cgi.o \
			$(COMMON_PATH)/make_log.o \
			$(COMMON_PATH)/cfg.o \
			$(COMMON_PATH)/md5.o
	$(CXX) $^ -o $@ $(LIBS)

$(sharefiles):	$(CGI_SRC_PATH)/sharefiles_cgi.o \
			$(COMMON_PATH)/cpp_cgi.o \
			$(COMMON_PATH)/make_log.o \
			$(COMMON_PATH)/cfg.o \
			$(COMMON_PATH)/md5.o
	$(CXX) $^ -o $@ $(LIBS)

$(dealsharefile):	$(CGI_SRC_PATH)/dealsharefile_cgi.o \
			$(COMMON_PATH)/cpp_cgi.o \
			$(COMMON_PATH)/make_log.o \
			$(COMMON_PATH)/cfg.o \
			$(COMMON_PATH)/md5.o
	$(CXX) $^ -o $@ $(LIBS)

$(sharepicture):	$(CGI_SRC_PATH)/sharepicture_cgi.o \
			$(COMMON_PATH)/cpp_cgi.o \
			$(COMMON_PATH)/make_log.o \
			$(COMMON_PATH)/cfg.o \
			$(COMMON_PATH)/md5.o
	$(CXX) $^ -o $@ $(LIBS)

$(chunk_init):	$(CGI_SRC_PATH)/chunk_init_cgi.o \
			$(COMMON_PATH)/cpp_cgi.o \
			$(COMMON_PATH)/make_log.o \
			$(COMMON_PATH)/cfg.o \
			$(COMMON_PATH)/md5.o
	$(CXX) $^ -o $@ $(LIBS)

$(chunk_upload):	$(CGI_SRC_PATH)/chunk_upload_cgi.o \
			$(COMMON_PATH)/cpp_cgi.o \
			$(COMMON_PATH)/make_log.o \
			$(COMMON_PATH)/cfg.o \
			$(COMMON_PATH)/md5.o
	$(CXX) $^ -o $@ $(LIBS)

$(chunk_merge):	$(CGI_SRC_PATH)/chunk_merge_cgi.o \
			$(COMMON_PATH)/cpp_cgi.o \
			$(COMMON_PATH)/make_log.o \
			$(COMMON_PATH)/cfg.o \
			$(COMMON_PATH)/md5.o
	$(CXX) $^ -o $@ $(LIBS)

$(ai): $(CGI_SRC_PATH)/ai_cgi.o \
	   $(CGI_SRC_PATH)/dashscope_api.o \
	   $(CGI_SRC_PATH)/faiss_wrapper.o \
	   $(COMMON_PATH)/cpp_cgi.o \
	   $(COMMON_PATH)/make_log.o \
	   $(COMMON_PATH)/cfg.o \
	   $(COMMON_PATH)/md5.o
	$(CXX) $^ -o $@ $(LIBS) $(AI_LIBS)

#######################################################################
# C++ object rules
$(CGI_SRC_PATH)/ai_cgi.o: $(CGI_SRC_PATH)/ai_cgi.cpp
	$(CXX) -c $< -o $@ $(CXXFLAGS) $(CPPLFAGS)

$(CGI_SRC_PATH)/upload_cgi.o: $(CGI_SRC_PATH)/upload_cgi.cpp
	$(CXX) -c $< -o $@ $(CXXFLAGS) $(CPPLFAGS)

$(CGI_SRC_PATH)/myfiles_cgi.o: $(CGI_SRC_PATH)/myfiles_cgi.cpp
	$(CXX) -c $< -o $@ $(CXXFLAGS) $(CPPLFAGS)
$(CGI_SRC_PATH)/sharefiles_cgi.o: $(CGI_SRC_PATH)/sharefiles_cgi.cpp
	$(CXX) -c $< -o $@ $(CXXFLAGS) $(CPPLFAGS)
$(CGI_SRC_PATH)/dealfile_cgi.o: $(CGI_SRC_PATH)/dealfile_cgi.cpp
	$(CXX) -c $< -o $@ $(CXXFLAGS) $(CPPLFAGS)
$(CGI_SRC_PATH)/dealsharefile_cgi.o: $(CGI_SRC_PATH)/dealsharefile_cgi.cpp
	$(CXX) -c $< -o $@ $(CXXFLAGS) $(CPPLFAGS)
$(CGI_SRC_PATH)/sharepicture_cgi.o: $(CGI_SRC_PATH)/sharepicture_cgi.cpp
	$(CXX) -c $< -o $@ $(CXXFLAGS) $(CPPLFAGS)

$(CGI_SRC_PATH)/login_cgi.o: $(CGI_SRC_PATH)/login_cgi.cpp
	$(CXX) -c $< -o $@ $(CXXFLAGS) $(CPPLFAGS)

$(CGI_SRC_PATH)/reg_cgi.o: $(CGI_SRC_PATH)/reg_cgi.cpp
	$(CXX) -c $< -o $@ $(CXXFLAGS) $(CPPLFAGS)

$(COMMON_PATH)/cpp_cgi.o: $(COMMON_PATH)/cpp_cgi.cpp
	$(CXX) -c $< -o $@ $(CXXFLAGS) $(CPPLFAGS)

$(COMMON_PATH)/make_log.o: $(COMMON_PATH)/make_log.cpp
	$(CXX) -c $< -o $@ $(CXXFLAGS) $(CPPLFAGS)

$(COMMON_PATH)/cfg.o: $(COMMON_PATH)/cfg.cpp
	$(CXX) -c $< -o $@ $(CXXFLAGS) $(CPPLFAGS)

$(CGI_SRC_PATH)/md5_cgi.o: $(CGI_SRC_PATH)/md5_cgi.cpp
	$(CXX) -c $< -o $@ $(CXXFLAGS) $(CPPLFAGS)

$(CGI_SRC_PATH)/chunk_init_cgi.o: $(CGI_SRC_PATH)/chunk_init_cgi.cpp
	$(CXX) -c $< -o $@ $(CXXFLAGS) $(CPPLFAGS)

$(CGI_SRC_PATH)/chunk_upload_cgi.o: $(CGI_SRC_PATH)/chunk_upload_cgi.cpp
	$(CXX) -c $< -o $@ $(CXXFLAGS) $(CPPLFAGS)

$(CGI_SRC_PATH)/chunk_merge_cgi.o: $(CGI_SRC_PATH)/chunk_merge_cgi.cpp
	$(CXX) -c $< -o $@ $(CXXFLAGS) $(CPPLFAGS)

$(CGI_SRC_PATH)/dashscope_api.o: $(COMMON_PATH)/dashscope_api.cpp
	$(CXX) -c $< -o $@ $(CXXFLAGS) $(CPPLFAGS)

$(CGI_SRC_PATH)/faiss_wrapper.o: $(COMMON_PATH)/faiss_wrapper.cpp
	$(CXX) -c $< -o $@ $(CXXFLAGS) $(CPPLFAGS)

#######################################################################
# Generic C object rule
%.o:%.c
	$(CC) -c $< -o $@ $(CPPLFAGS) $(CFLAGS)

clean:
	-rm -rf *.o $(target) $(TEST_PATH)/*.o $(CGI_SRC_PATH)/*.o $(COMMON_PATH)/*.o

.PHONY:clean ALL




