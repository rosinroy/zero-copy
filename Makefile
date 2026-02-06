CXX = g++
CXXFLAGS = -std=c++17 -Wall -O2

GST_CFLAGS := $(shell pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0)
GST_LIBS   := $(shell pkg-config --libs gstreamer-1.0 gstreamer-app-1.0)

NVBUF_CFLAGS = -I/opt/nvidia/deepstream/deepstream-7.0/sources/includes
NVBUF_LIBS = -L/opt/nvidia/deepstream/deepstream-7.0/lib \
             -lnvbufsurface -lnvbufsurftransform

all: producer consumer

producer:main.cpp
	$(CXX) $(CXXFLAGS) $(GST_CFLAGS) $(NVBUF_CFLAGS) \
		main.cpp -o producer $(GST_LIBS)

consumer: consumer.cpp
	$(CXX) $(CXXFLAGS) $(NVBUF_CFLAGS) \
		consumer.cpp -o consumer $(NVBUF_LIBS)

clean:
	rm -f producer consumer
