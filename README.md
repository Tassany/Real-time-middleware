# Real-time-middleware
This is a reproduction of a paper titled “MCFlow: A Real-Time Multi-core Aware Middleware for Dependent Task Graphs”. 


## Install the library

mkdir -p include/nlohmann
curl -L https://github.com/nlohmann/json/releases/latest/download/json.hpp \
     -o include/nlohmann/json.hpp

## Compile and execute the tests:
  make                   
  make examples           
  make example_dispatcher 
  make clean             
  

## Run
  ./main

## Exemplos
  ./example_ring
  ./example_eventfd
  ./example_two_threads
  ./example_epoll
   sudo ./example_dispatcher

