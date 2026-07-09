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



  # 1. Extrai o CFG (gera bs.xml)
  bash extract.sh bs binary_search MIPS MIPS_bs

  # 2. Calcula o WCET (imprime "WCET: 3889" no final)
  bash analysis.sh bs binary_search MIPS lp_solve MIPS_bs

  cp /caminho/para/minha_fn.c benchmarks/minha_fn/minha_fn.c
  bash extract.sh minha_fn nome_da_fn MIPS MIPS_out
  bash analysis.sh minha_fn nome_da_fn MIPS lp_solve MIPS_out
