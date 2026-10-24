CXX      := clang++
CXXFLAGS := -std=c++20 -O2 -fno-math-errno -Wall -Wextra

CORE_SRCS := \
	framebuffer.cpp \
	spanfill_neon.cpp \
	spanfill_avx2.cpp \
	track.cpp \
	roadrender.cpp \
	sprites.cpp \
	cockpit.cpp \
	vehicle.cpp \
	collision.cpp \
	audio.cpp \
	jobs.cpp \
	highscores.cpp \
	game.cpp \
	tests.cpp \
	main.cpp

all: racer_tests

racer_tests: $(CORE_SRCS)
	$(CXX) $(CXXFLAGS) -DRACER_NO_QT $(CORE_SRCS) -o racer_tests

test: racer_tests
	./racer_tests --test

clean:
	rm -f racer_tests
	rm -rf build
