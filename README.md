# Real-time-middleware
This is a reproduction of a paper titled “MCFlow: A Real-Time Multi-core Aware Middleware for Dependent Task Graphs”. 


## Install the library

mkdir -p include/nlohmann


curl -L https://github.com/nlohmann/json/releases/latest/download/json.hpp \
     -o include/nlohmann/json.hpp


make       
make clean 

## Run the code

sudo ./example_from_plan deployment_plan.json

## Evaluation of the tasks
 sudo ./example_eval deployment_plan.json

## Run plot the results
python3 plot_latency.py latency_samples.csv