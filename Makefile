CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Iinclude

TARGET = main
SRCS   = main.cc parser_json.cpp
HDRS   = component.hpp adapter.hpp dag.hpp dispacher.hpp \
         deployment_plan.hpp parser_json.hpp

$(TARGET): $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

clean:
	rm -f $(TARGET)

.PHONY: clean
