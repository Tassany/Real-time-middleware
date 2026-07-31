/*
 * etapa8.cpp — ciclos medidos de verdade, com PMCCNTR_EL0.
 *
 * A etapa 7 fechou com um problema que ela nao podia resolver: quatro dos seis
 * benchmarks medem entre 12 e 46 ticks, e um tick vale 44 ciclos de nucleo. A
 * regua e' grossa demais.
 *
 * Esta etapa troca a regua. O PMCCNTR_EL0 conta CICLOS DE NUCLEO, um a um, o
 * que da' 44 vezes mais resolucao e mede diretamente a grandeza que o paper
 * reporta, em vez de deriva-la de uma medicao de tempo.
 *
 * EXIGE O MODULO DE KERNEL de etapa8_pmu/ carregado. Sem ele a instrucao de
 * leitura gera SIGILL. O programa detecta isso e explica, em vez de morrer com
 * "Illegal instruction" e deixar voce adivinhando.
 *
 * O QUE ESTA ETAPA MEDE, e e' o ponto:
 *
 * Os dois contadores sao lidos na MESMA janela, com uma unica barreira. Assim
 * cada execucao produz um par: quantos ticks de generic timer e quantos ciclos
 * de nucleo. A razao entre o valor medido e o valor derivado
 * (ticks x f_nucleo / f_timer) testa empiricamente a suposicao em que toda a
 * etapa 7 se apoiava. Se der 1,00, a conversao estava certa. Se nao der, voce
 * descobre por quanto ela errava.
 *
 * ADVERTENCIA que vem junto com a resolucao melhor: o contador de ciclos e' POR
 * NUCLEO, e nucleos diferentes tem valores diferentes e nao sincronizados. Se a
 * thread migrar entre as duas leituras, a diferenca e' lixo, possivelmente
 * negativo. O `-c` da etapa 6 deixa de ser refinamento e passa a ser requisito.
 * O programa verifica em que nucleo terminou e reclama se nao for o pedido.
 *
 * Um binario por benchmark:  bin8/bsort100, bin8/crc, ...
 *
 * Uso:  ./bin8/<nome> [-n EXEC] [-w AQUECIMENTOS] [-c NUCLEO] [--header]
 *
 * ---------------------------------------------------------------------------
 * O que segue abaixo veio da etapa 7 e continua valendo.
 * ---------------------------------------------------------------------------
 *
 * etapa7.cpp — de ticks para tempo, de tempo para ciclos, e uma linha de CSV.
 *
 * A etapa 6 entregou medicoes reprodutiveis. Falta responder a pergunta que o
 * paper de fato faz, que e' em CICLOS DE PROCESSADOR, e nao em ticks nem em
 * microssegundos.
 *
 * O ponto central, e onde o paper e' impreciso:
 *
 *     CNTVCT_EL0 NAO CONTA CICLOS DE PROCESSADOR.
 *
 * Ele e' o generic timer. Tica a CNTFRQ_EL0, 54 MHz nesta placa, fixos, gravados
 * pelo firmware no boot. O nucleo, enquanto isso, roda a 1,5, 1,8 ou 2,4 GHz
 * conforme o governor decide. Sao dois relogios independentes.
 *
 * Consequencia, e a etapa 6 mostrou isso com dados:
 *
 *     ticks -> nanossegundos   e' EXATO.  ns = ticks * 1e9 / f_timer
 *                              O f_timer e' constante e conhecido.
 *
 *     ticks -> ciclos          e' DERIVADO.  ciclos = ticks * f_nucleo / f_timer
 *                              Depende de f_nucleo, que muda, e a formula supoe
 *                              que ele ficou parado durante todo o lote.
 *
 * A validacao empirica que voce ja' produziu, medindo o matmult em tres clocks:
 *
 *     1500 MHz -> 4156 ticks -> 115.444 ciclos
 *     1800 MHz -> 3459 ticks -> 115.300 ciclos
 *     2400 MHz -> 2595 ticks -> 115.333 ciclos
 *
 * Os ticks variam 38%, os ciclos concordam em 0,125%. A conversao funciona, e a
 * quantidade invariante e' a contagem de ciclos. Mas repare no que ela EXIGE:
 * saber f_nucleo, e que ele nao tenha se mexido. Por isso este programa le' a
 * frequencia antes e depois do lote e recusa a coluna de ciclos se ela mudou.
 *
 * NOVIDADE DE FORMATO: a saida se divide em dois canais.
 *
 *   stdout   uma linha de CSV, legivel por maquina, para juntar os seis
 *            benchmarks numa tabela so'
 *   stderr   o relatorio legivel por gente, igual ao das etapas anteriores
 *
 * Assim `./bin7/crc >> tabela.csv` funciona sem que o relatorio suje o arquivo,
 * e voce continua vendo os avisos na tela. E' o que o alvo `make medir` usa.
 *
 * Um binario por benchmark:  bin7/bsort100, bin7/crc, ...
 *
 * Uso:  ./bin7/<nome> [-n EXEC] [-w AQUECIMENTOS] [-c NUCLEO] [--header]
 */

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <csetjmp>
#include <algorithm>

#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef BENCH_NAME
#define BENCH_NAME "desconhecido"
#endif

/* ================================================================== */
/* O PROBLEMA: dois main() nao cabem num programa                     */
/* ================================================================== */
/*
 * Cada benchmark Malardalen e' um programa completo, com seu proprio main().
 * Este arquivo tambem tem um main(). Se voce simplesmente compilar os dois
 * juntos, o linker reclama de simbolo duplicado e para.
 *
 * COMPILACAO SEPARADA, primeiro, porque tudo aqui depende disso.
 *
 * Compilar e linkar sao duas etapas distintas. O compilador processa UM arquivo
 * por vez e produz um arquivo-objeto (.o) contendo codigo de maquina mais uma
 * tabela de simbolos: os nomes que aquele arquivo DEFINE e os que ele USA sem
 * definir. O linker vem depois, junta todos os .o e casa cada nome usado com
 * a sua definicao. Se um nome tem duas definicoes, ele nao sabe qual escolher.
 * Se um nome nao tem nenhuma, e' o famoso "undefined reference".
 *
 * A consequencia util: bsort100.c e etapa5.cpp nunca se veem. Cada um e'
 * compilado sozinho, e o unico contrato entre eles e' um nome de funcao.
 *
 * A SOLUCAO: renomear main() na linha de comando.
 *
 *     gcc -Dmain=bench_entry -c ../bsort100.c
 *
 * -D define uma macro de pre-processador, exatamente como um #define no topo do
 * arquivo, so' que sem tocar no arquivo. O pre-processador troca toda ocorrencia
 * do texto `main` por `bench_entry` antes do compilador ver o codigo. O
 * `main()` do bsort100 vira `bench_entry()`, uma funcao comum. O arquivo em
 * disco continua intacto, e e' por isso que este tutorial nao precisa de copias
 * modificadas das fontes.
 *
 * E O extern "C", que e' o pedaco que so' existe porque este arquivo e' C++.
 *
 * C++ permite sobrecarga: duas funcoes podem se chamar `f` se receberem tipos
 * diferentes. Para isso funcionar, o compilador C++ codifica os tipos dos
 * parametros dentro do nome do simbolo -- e' o NAME MANGLING. `void f(int)`
 * vira algo como `_Z1fi` no arquivo-objeto.
 *
 * O compilador C nao faz nada disso. `bench_entry` no bsort100.o e' literalmente
 * `bench_entry`.
 *
 * Se este arquivo declarasse `void bench_entry();` sem mais nada, o g++
 * procuraria `_Z11bench_entryv` e o linker falharia. `extern "C"` desliga o
 * mangling para aquela declaracao: procure o nome cru, como o C escreveria.
 *
 * DETALHE FINO, sobre o tipo de retorno.
 *
 * As seis fontes discordam entre si: fft1.c, matmult.c e ud.c declaram
 * `void main()`, enquanto crc.c e statemate.c declaram `int main(void)`, e
 * bsort100.c escreve so' `main()`, que em C89 significa retorno int implicito.
 *
 * Nao da' para declarar todos com um tipo so' e estar certa em todos os casos.
 * Declaramos como void e descartamos o retorno quando ele existe. Na convencao
 * de chamada do AArch64 o valor de retorno vem no registrador x0, e ignorar x0
 * e' inofensivo -- quem chama simplesmente nao le'. A alternativa (declarar int
 * e ler x0 mesmo quando a funcao nao escreveu nada nele) seria pior, porque
 * leria lixo.
 */

extern "C" void bench_entry();

/* ================================================================== */
/* bsort100: um endereco de memoria cravado na fonte                  */
/* ================================================================== */
/*
 * bsort100.c abre com isto:
 *
 *     #define KNOWN_VALUE (int)(*((char *)0x80200001))
 *
 * e usa a macro em Initialize(). Ou seja, o benchmark LE' O ENDERECO ABSOLUTO
 * 0x80200001 esperando encontrar o valor 1 la' dentro. O comentario da fonte
 * diz a intencao: "uma leitura deste endereco resultara' num valor conhecido
 * de 1".
 *
 * Isso funcionava no ambiente para o qual o benchmark foi escrito, um simulador
 * ou uma placa sem sistema operacional, onde o programa enxerga o mapa de
 * memoria fisica cru e aquele endereco tinha um valor fixo cravado.
 *
 * Num processo Linux nao existe mapa fisico. Cada processo tem um ESPACO DE
 * ENDERECAMENTO VIRTUAL proprio, e o kernel so' liga um endereco virtual a
 * memoria fisica quando alguem pede. Enderecos que ninguem pediu ficam sem
 * traducao, e le-los gera uma falha de pagina que o kernel nao sabe resolver.
 * O resultado e' SIGSEGV, que e' o "Segmentation fault" que voce viu.
 *
 * O conserto poderia ser editar a fonte e trocar a macro por 1. Nos nao vamos
 * fazer isso, porque a fonte intocada e' o que garante que voce esta' medindo o
 * mesmo programa que os outros trabalhos da area medem. Em vez disso, pedimos
 * ao kernel a pagina que falta:
 *
 *     mmap(0x80200000, 4096, ...) e escrevemos 1 no byte 1.
 *
 * mmap com MAP_FIXED_NOREPLACE pede um endereco ESPECIFICO, e falha em vez de
 * mover a mapeamento para outro lugar se aquela regiao ja' estiver ocupada. O
 * endereco precisa ser multiplo do tamanho da pagina, e 0x80200000 e'.
 * Depois disso, ler 0x80200001 devolve 1, exatamente como o benchmark supunha,
 * e o programa em disco continua intocado.
 */
#ifdef MAPEAR_ENDERECO_FIXO

/* MAP_FIXED_NOREPLACE existe a partir do Linux 4.17. Se a libc for antiga,
 * cai para MAP_FIXED, que faz a mesma coisa mas sobrescreve em silencio um
 * mapeamento existente em vez de reclamar. */
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE MAP_FIXED
#endif

static void mapear_endereco_fixo()
{
	void *base = (void *)0x80200000UL;
	size_t tam = (size_t)sysconf(_SC_PAGESIZE);

	void *p = mmap(base, tam, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);

	if (p == MAP_FAILED || p != base) {
		fprintf(stderr,
			"erro: nao consegui mapear a pagina em 0x80200000 que o "
			"bsort100 le'.\n");
		exit(1);
	}

	unsigned char *b = (unsigned char *)p;

	b[1] = 1;   /* KNOWN_VALUE, o unico que bsort100 usa */
	b[3] = 1;   /* UNKNOWN_VALUE, definido na fonte mas nunca chamado */
}

#else
static void mapear_endereco_fixo() { }
#endif

/* ------------------------------------------- instrumento (vem da etapa 3) */

static inline uint64_t ler_contador()
{
	uint64_t v;

	asm volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v) : : "memory");
	return v;
}

static inline uint64_t ler_cntfrq()
{
	uint64_t v;

	asm volatile("mrs %0, cntfrq_el0" : "=r"(v));
	return v;
}

static inline void barreira()
{
	asm volatile("" : : : "memory");
}

/* ================================================================== */
/* O CONTADOR DE CICLOS, E COMO DESCOBRIR SE ELE ESTA' DISPONIVEL     */
/* ================================================================== */
/*
 * Ler os dois contadores de uma vez, com UMA barreira so'.
 *
 * Poderiam ser duas funcoes chamadas em sequencia, mas cada `isb` custa dezenas
 * de ciclos, e duas barreiras separariam as leituras no tempo. Aqui um unico
 * isb serializa, e as duas instrucoes `mrs` saem coladas. O desalinhamento entre
 * o que cada contador registra fica reduzido a uma instrucao.
 *
 * Repare nos dois operandos de saida: %0 e %1, listados na ordem em que
 * aparecem depois dos dois pontos. O compilador escolhe dois registradores
 * livres e substitui os dois marcadores.
 */
static inline void ler_ambos(uint64_t *ticks, uint64_t *ciclos)
{
	asm volatile("isb\n\t"
		     "mrs %0, cntvct_el0\n\t"
		     "mrs %1, pmccntr_el0"
		     : "=r"(*ticks), "=r"(*ciclos)
		     :
		     : "memory");
}

/*
 * Descobrir se o modulo esta' carregado, sem morrer no processo.
 *
 * Sem a autorizacao do PMUSERENR_EL0, a instrucao `mrs x0, pmccntr_el0` nao le'
 * lixo nem devolve zero: ela dispara uma excecao de instrucao ilegal, que o
 * Linux entrega ao processo como o sinal SIGILL. O padrao para SIGILL e'
 * terminar o programa. Rodar sem o modulo daria "Illegal instruction" e nada
 * mais.
 *
 * A saida e' instalar um TRATADOR para o sinal e usar sigsetjmp/siglongjmp:
 *
 *   sigsetjmp   grava o estado atual (registradores, pilha, mascara de sinais)
 *               e devolve 0 na primeira passagem
 *   siglongjmp  volta para aquele ponto de dentro do tratador, e faz o
 *               sigsetjmp devolver o valor passado, desta vez diferente de zero
 *
 * E' a unica forma limpa de continuar depois de um SIGILL, porque retornar
 * normalmente do tratador reexecutaria a mesma instrucao ilegal, num laco
 * infinito. O `1` no segundo argumento do sigsetjmp pede que a mascara de
 * sinais seja salva e restaurada, sem o que SIGILL ficaria bloqueado depois do
 * salto, ja' que o kernel o bloqueia enquanto o tratador roda.
 */

static sigjmp_buf ponto_de_retorno;

static void tratador_sigill(int)
{
	siglongjmp(ponto_de_retorno, 1);
}

static bool pmu_acessivel()
{
	struct sigaction nova, antiga;

	memset(&nova, 0, sizeof(nova));
	nova.sa_handler = tratador_sigill;
	sigemptyset(&nova.sa_mask);

	if (sigaction(SIGILL, &nova, &antiga) != 0)
		return false;

	bool ok = true;

	if (sigsetjmp(ponto_de_retorno, 1) == 0) {
		uint64_t v;

		asm volatile("mrs %0, pmccntr_el0" : "=r"(v));
		barreira();
	} else {
		ok = false;      /* chegamos aqui vindos do tratador */
	}

	sigaction(SIGILL, &antiga, nullptr);
	return ok;
}

/*
 * Acessivel nao e' o mesmo que funcionando. Se PMCR_EL0.E estiver desligado, ou
 * se o bit do contador de ciclos nao tiver sido habilitado em PMCNTENSET_EL0, a
 * leitura e' permitida e devolve sempre o mesmo valor. Duas leituras com
 * trabalho no meio detectam isso.
 */
static bool pmu_contando()
{
	uint64_t t, a, b;
	volatile uint64_t lixo = 0;

	ler_ambos(&t, &a);
	for (int i = 0; i < 10000; i++)
		lixo += (uint64_t)i;
	ler_ambos(&t, &b);

	(void)lixo;
	return b > a;
}

static uint64_t medir_piso_pmu()
{
	uint64_t menor = UINT64_MAX;

	for (int i = 0; i < 1000; i++) {
		uint64_t t1, t2, c1, c2;

		ler_ambos(&t1, &c1);
		ler_ambos(&t2, &c2);
		if (c2 - c1 < menor)
			menor = c2 - c1;
	}
	return menor;
}

/* ================================================================== */
/* ESTADO ENTRE EXECUCOES — a licao A da etapa 4, agora de verdade    */
/* ================================================================== */
/*
 * Na etapa 4 nos chamavamos preparar() antes de cada execucao, para nao medir
 * quatro vezes o melhor caso. Aqui nao da': o estado esta' dentro do benchmark,
 * em variaveis globais e estaticas que este arquivo nem enxerga.
 *
 * O que cada uma das seis fontes faz, depois de ler o codigo delas:
 *
 *   bsort100   main() chama Initialize(), que reescreve o vetor inteiro em
 *              ordem invertida. Restaura-se sozinho. OK.
 *   matmult    main() chama InitSeed(), e Test() regenera as matrizes a partir
 *              da semente. Restaura-se sozinho. OK.
 *   ud         main() preenche a[][] e b[] no proprio corpo. OK.
 *   statemate  main() chama init(). OK.
 *
 *   crc        NAO. icrc() tem `static unsigned short icrctb[256], init=0;` e
 *              constroi a tabela de 256 entradas apenas na PRIMEIRA chamada.
 *              Da segunda em diante o `if (!init)` e' falso e o trabalho some.
 *              Logo: a execucao 1 e' muito mais cara que as execucoes 2..5, e
 *              se voce aquecer antes de medir, a construcao da tabela cai
 *              inteira no aquecimento e NENHUMA amostra medida a inclui.
 *
 *   fft1       PARCIALMENTE. main() reescreve ar[] a cada chamada, mas nao
 *              ai[], que fica com a saida da fft anterior. Da segunda execucao
 *              em diante o benchmark opera sobre dados diferentes. A estrutura
 *              de lacos do fft1 nao depende dos dados, entao o tempo tende a
 *              ser estavel, mas os valores nao sao os mesmos e vale conferir se
 *              a amostra 1 destoa.
 *
 * Nao ha' conserto limpo dentro deste desenho: zerar `init` do crc de fora e'
 * impossivel, porque e' uma estatica local. O conserto de verdade seria uma
 * medicao por processo, relancando o binario a cada amostra. Este programa em
 * vez disso torna o problema VISIVEL: com `-w 0` voce mede sem aquecimento e ve
 * a amostra 1 destoar; comparando com `-w 1` voce ve exatamente quanto da'
 * medicao o aquecimento levou embora.
 */

/* ================================================================== */
/* ISOLAMENTO 1 — prender a thread a um nucleo                        */
/* ================================================================== */
/*
 * Por padrao o escalonador do Linux pode mover sua thread de nucleo a qualquer
 * momento, e ele faz isso o tempo todo para equilibrar carga. Cada mudanca
 * custa caro na medicao:
 *
 *   - CACHE PERDIDA. A Pi 5 tem quatro Cortex-A76, cada um com sua L1 de 64 KiB
 *     e sua L2 de 512 KiB privadas. Ao migrar, sua thread chega num nucleo onde
 *     nada do seu programa esta' em cache, e recomeca fria.
 *   - PREDITOR DE SALTOS PERDIDO, pelo mesmo motivo.
 *
 * Uma coisa que a migracao NAO estraga, e vale saber por contraste: o valor do
 * CNTVCT_EL0. O generic timer do ARM e' alimentado por um contador de sistema
 * unico, externo aos nucleos, e a arquitetura exige que todos os nucleos vejam
 * o mesmo valor. Ler antes num nucleo e depois noutro continua dando uma
 * diferenca correta. No x86 o contador equivalente e' por nucleo e essa mesma
 * migracao produziria lixo, as vezes ate' negativo. Voce esta' com sorte de
 * arquitetura aqui.
 *
 * A CORRECAO: sched_setaffinity().
 *
 * A "afinidade" de uma thread e' o conjunto de nucleos onde ela pode rodar. Por
 * padrao sao todos. O tipo cpu_set_t e' uma mascara de bits, um bit por nucleo,
 * manipulada pelas macros CPU_ZERO (limpa tudo) e CPU_SET (liga um bit).
 * Reduzindo o conjunto a um unico nucleo, o escalonador nao tem mais para onde
 * levar a thread.
 *
 * Nao precisa de sudo: reduzir a propria afinidade e' permitido a qualquer
 * processo.
 *
 * O LIMITE DISSO: prender a SUA thread a um nucleo nao impede o resto do
 * sistema de usar aquele mesmo nucleo. Interrupcoes e outras tarefas continuam
 * chegando la'. O isolamento de verdade e' um parametro de boot,
 * `isolcpus=3 nohz_full=3 irqaffinity=0-2` no cmdline.txt, que retira o nucleo
 * 3 do escalonador comum e desvia as interrupcoes para os outros. Isso esta'
 * fora do escopo deste tutorial, mas e' o proximo passo se voce precisar de
 * numeros melhores que os que vamos obter aqui.
 */

static bool prender_no_nucleo(int nucleo)
{
	cpu_set_t conjunto;

	CPU_ZERO(&conjunto);
	CPU_SET(nucleo, &conjunto);

	return sched_setaffinity(0, sizeof(conjunto), &conjunto) == 0;
}

/* ================================================================== */
/* ISOLAMENTO 2 — nao ser interrompida pelo escalonador               */
/* ================================================================== */
/*
 * Prender a thread num nucleo nao impede que ela seja TIRADA dele. A politica
 * padrao do Linux e' SCHED_OTHER, atendida pelo CFS, que reparte o tempo entre
 * todas as tarefas prontas. Sua thread ganha uma fatia, e quando a fatia acaba
 * ela sai e outra entra, mesmo que a sua tenha trabalho a fazer. Se isso cair
 * no meio da janela medida, o tempo da outra tarefa entra na sua medicao.
 *
 * A CORRECAO: SCHED_FIFO.
 *
 * O Linux tem politicas de TEMPO REAL, e SCHED_FIFO e' a mais simples delas:
 * uma thread FIFO roda ate' bloquear ou ceder voluntariamente, e so' pode ser
 * preemptada por outra thread de tempo real com prioridade MAIOR. Nenhuma
 * tarefa comum, de qualquer tipo, interrompe uma thread FIFO. As prioridades
 * vao de 1 a 99.
 *
 * EXIGE PRIVILEGIO (a capacidade CAP_SYS_NICE, na pratica sudo), e a razao e'
 * obvia: uma thread FIFO em laco infinito monopolizaria um nucleo. O Linux tem
 * uma rede de seguranca, a limitacao de banda de tempo real, que por padrao
 * concede no maximo 950 ms de cada 1000 ms as tarefas de tempo real
 * (/proc/sys/kernel/sched_rt_runtime_us). Ou seja, mesmo em FIFO voce leva uma
 * interrupcao forcada de 50 ms a cada segundo. Para lotes curtos como os
 * nossos isso raramente aparece.
 *
 * Se falhar, este programa DIZ que falhou e continua. Reportar um numero sem
 * avisar que o isolamento nao aconteceu seria pior do que nao ter tentado.
 */

static bool virar_tempo_real(int prioridade)
{
	struct sched_param p;

	memset(&p, 0, sizeof(p));
	p.sched_priority = prioridade;

	return sched_setscheduler(0, SCHED_FIFO, &p) == 0;
}

/* ================================================================== */
/* ISOLAMENTO 3 — nenhuma falta de pagina dentro da janela            */
/* ================================================================== */
/*
 * O Linux entrega memoria preguicosamente. Quando seu programa reserva um
 * vetor, o kernel promete o espaco no espaco de enderecamento virtual mas nao
 * entrega RAM fisica. A entrega acontece no primeiro acesso, atraves de uma
 * FALTA DE PAGINA: o hardware percebe que aquele endereco virtual nao tem
 * traducao, gera uma excecao, o kernel arruma uma pagina fisica, atualiza a
 * tabela de paginas e devolve o controle. Custa microssegundos.
 *
 * Se uma falta dessas cair dentro da janela medida, ela aparece como tempo de
 * execucao do benchmark. E' o mesmo fenomeno que o aquecimento da etapa 4
 * combate, so' que aqui de forma explicita e permanente.
 *
 * A CORRECAO: mlockall(MCL_CURRENT | MCL_FUTURE).
 *
 *   MCL_CURRENT   traz para a RAM e fixa la' tudo que ja' esta' mapeado agora
 *   MCL_FUTURE    faz o mesmo automaticamente para tudo que for mapeado depois
 *
 * Fixado significa que o kernel nao pode mandar aquelas paginas para a swap nem
 * desfazer o mapeamento. Como consequencia, nenhum acesso a memoria do seu
 * processo pode gerar falta de pagina.
 *
 * Precisa de privilegio na pratica: o limite RLIMIT_MEMLOCK de um usuario comum
 * costuma ser de poucos megabytes, e como root e' ilimitado.
 */

static bool fixar_memoria()
{
	return mlockall(MCL_CURRENT | MCL_FUTURE) == 0;
}

/* ================================================================== */
/* ISOLAMENTO 4 — o clock do nucleo                                   */
/* ================================================================== */
/*
 * A maior fonte de erro que a etapa 5 revelou, e a unica que este programa NAO
 * conserta sozinho.
 *
 * O governor de frequencia decide, dezenas de vezes por segundo, a que clock
 * cada nucleo roda. Com `ondemand`, o padrao da Raspberry Pi OS, ele sobe
 * quando ve carga e desce quando nao ve. Um lote de cinco execucoes de poucos
 * microssegundos e' curto demais para ele reagir, entao o lote inteiro roda
 * no clock em que a maquina JA' ESTAVA -- que depende do que voce fez antes.
 * Foi isso que produziu a razao de 1,598 entre duas rodadas do matmult.
 *
 * Por que o programa nao arruma sozinho: escrever em scaling_governor muda o
 * comportamento da maquina inteira e continua valendo depois que o benchmark
 * termina. Um programa de medicao que altera o sistema em silencio e' pior que
 * um que so' reporta. Entao aqui nos LEMOS e DENUNCIAMOS.
 *
 * A leitura acontece ANTES e DEPOIS do lote. Se a frequencia mudou no meio, o
 * lote e' misturado e o programa avisa. Isso vai importar ainda mais na
 * etapa 7, onde converter ticks em ciclos exige justamente supor que o clock
 * ficou parado.
 */

static uint64_t ler_u64_sysfs(const char *caminho)
{
	FILE *f = fopen(caminho, "r");
	unsigned long long v = 0;

	if (!f)
		return 0;
	if (fscanf(f, "%llu", &v) != 1)
		v = 0;
	fclose(f);
	return (uint64_t)v;
}

/* Frequencia atual do nucleo, em Hz. O sysfs reporta em kHz. */
static uint64_t freq_nucleo_hz(int nucleo)
{
	char caminho[160];

	snprintf(caminho, sizeof(caminho),
		 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", nucleo);
	return ler_u64_sysfs(caminho) * 1000ULL;
}

static void ler_governor(int nucleo, char *destino, size_t tam)
{
	char caminho[160];

	snprintf(caminho, sizeof(caminho),
		 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", nucleo);

	FILE *f = fopen(caminho, "r");

	if (!f || !fgets(destino, (int)tam, f)) {
		snprintf(destino, tam, "?");
	} else {
		destino[strcspn(destino, "\n")] = '\0';
	}
	if (f)
		fclose(f);
}

/* ------------------------------------------------------------------ main */

static void uso(const char *prog)
{
	fprintf(stderr,
		"uso: %s [-n EXEC] [-w AQUECIMENTOS] [-c NUCLEO] [--header]\n"
		"  -n EXEC           execucoes medidas (padrao 5, o numero do paper)\n"
		"  -w AQUECIMENTOS   execucoes descartadas antes (padrao 1)\n"
		"  -c NUCLEO         nucleo onde prender a thread (padrao 3)\n"
		"  --header          imprime so' o cabecalho do CSV e sai\n"
		"\n"
		"stdout leva uma linha de CSV; o relatorio legivel vai para stderr.\n"
		"Rode com sudo para conseguir SCHED_FIFO e mlockall.\n", prog);
}

/* ================================================================== */
/* PISO DO INSTRUMENTO — o vies que esta' dentro de toda amostra      */
/* ================================================================== */
/*
 * A etapa 3 mediu quanto custa o proprio par de leituras: duas chamadas
 * seguidas, sem nada no meio, minimo de mil tentativas. Aquele custo nao vai
 * embora quando existe um benchmark no meio. Ele esta' DENTRO de cada amostra,
 * somado ao tempo do programa.
 *
 * Para o matmult, 2 ticks em 2595 e' 0,08 % e some no arredondamento. Para o
 * statemate, 2 ticks em 12 sao 17 % do resultado. Isso e' erro de EXATIDAO, e
 * nao de precisao: repetir mais vezes nao o remove, porque ele nao e' ruido,
 * e' um deslocamento constante para cima.
 *
 * O programa mede e REPORTA o piso, sem subtrai-lo. Subtrair seria assumir que
 * a sobreposicao entre o custo do isb e o do benchmark e' nula, o que num
 * nucleo fora de ordem nao e' verdade. Reportar deixa a correcao a seu criterio
 * e deixa o vies visivel em vez de escondido.
 */

static uint64_t medir_piso()
{
	uint64_t menor = UINT64_MAX;

	for (int i = 0; i < 1000; i++) {
		uint64_t a = ler_contador();
		uint64_t b = ler_contador();

		if (b - a < menor)
			menor = b - a;
	}
	return menor;
}

/* As colunas do CSV. Ordem fixa: se voce precisar acrescentar alguma coisa
 * depois, ACRESCENTE NO FIM, para nao quebrar planilhas e scripts que ja'
 * dependem das posicoes atuais.
 *
 * `max_confiavel` substituiu a coluna que antes se chamava `utilizavel`. O nome
 * velho dava a entender que as medidas eram falsas quando ele valia 0, o que
 * nao e' o caso: as medidas sao boas, o que fica comprometido nos benchmarks
 * curtos e' especificamente a estatistica de MAXIMO, porque um tick de
 * arredondamento e uma execucao lenta ficam indistinguiveis. A media sobrevive,
 * porque a quantizacao e' aproximadamente uniforme e se cancela nela. */
static const char *CSV_CABECALHO =
	"benchmark,n,aquecimentos,nucleo,governor,"
	"f_nucleo_hz,f_timer_hz,clock_estavel,isolamento_completo,"
	"min_ticks,mediana_ticks,media_ticks,max_ticks,"
	"min_us,mediana_us,media_us,max_us,"
	"media_ciclos,mediana_ciclos,max_ciclos,"
	"resolucao_rel_pct,dispersao_pct,max_confiavel,"
	"piso_ticks,piso_pct_mediana,incerteza_media_pct,"
	/* acrescentado na etapa 8, sempre no fim */
	"pmu_ok,min_pmucyc,mediana_pmucyc,media_pmucyc,max_pmucyc,"
	"piso_pmucyc,razao_medido_derivado,resolucao_pmu_pct,dispersao_pmu_pct";

int main(int argc, char **argv)
{
	int n = 5;
	int aquecimentos = 1;
	int nucleo = 3;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--header")) {
			puts(CSV_CABECALHO);
			return 0;
		} else if (!strcmp(argv[i], "-n") && i + 1 < argc) {
			n = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "-w") && i + 1 < argc) {
			aquecimentos = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "-c") && i + 1 < argc) {
			nucleo = atoi(argv[++i]);
		} else {
			uso(argv[0]);
			return 2;
		}
	}
	if (n < 1 || n > 100000 || aquecimentos < 0 || nucleo < 0) {
		uso(argv[0]);
		return 2;
	}

	uint64_t f_timer = ler_cntfrq();
	double ns_por_tick = f_timer ? 1e9 / (double)f_timer : 0.0;

	/* ---- isolamento, antes de qualquer medicao ---- */
	bool ok_afinidade = prender_no_nucleo(nucleo);
	int  erro_afinidade = ok_afinidade ? 0 : errno;

	bool ok_fifo = virar_tempo_real(80);
	int  erro_fifo = ok_fifo ? 0 : errno;

	bool ok_mlock = fixar_memoria();
	int  erro_mlock = ok_mlock ? 0 : errno;

	char governor[64];
	ler_governor(nucleo, governor, sizeof(governor));

	uint64_t f_antes = freq_nucleo_hz(nucleo);

	mapear_endereco_fixo();

	/* ---- o contador de ciclos esta' disponivel? ---- */
	bool pmu_ok = pmu_acessivel();

	if (pmu_ok && !pmu_contando()) {
		fprintf(stderr,
			"[%s] aviso: PMCCNTR_EL0 e' legivel mas nao avanca. O modulo\n"
			"      autorizou o acesso mas o contador esta' parado (PMCR_EL0.E\n"
			"      ou PMCNTENSET_EL0). Recarregue: cd etapa8_pmu && make unload"
			" load\n", BENCH_NAME);
		pmu_ok = false;
	}
	if (!pmu_ok)
		fprintf(stderr,
			"[%s] aviso: PMCCNTR_EL0 inacessivel (SIGILL). O modulo de kernel\n"
			"      nao esta' carregado. Sem ele esta etapa reporta apenas as\n"
			"      colunas da etapa 7, derivadas do generic timer.\n"
			"      Para carregar:  cd etapa8_pmu && make load\n", BENCH_NAME);

	uint64_t piso = medir_piso();
	uint64_t piso_pmu = pmu_ok ? medir_piso_pmu() : 0;

	uint64_t *amostras = new uint64_t[n];
	uint64_t *ciclos_medidos = new uint64_t[n];

	for (int i = 0; i < aquecimentos; i++) {
		bench_entry();
		barreira();
	}

	/* Os dois contadores na mesma janela. Quando o PMU nao esta' disponivel
	 * caimos na leitura simples da etapa 7, para o programa continuar util. */
	for (int i = 0; i < n; i++) {
		uint64_t t0, t1, c0 = 0, c1 = 0;

		if (pmu_ok) {
			ler_ambos(&t0, &c0);
			bench_entry();
			ler_ambos(&t1, &c1);
		} else {
			t0 = ler_contador();
			bench_entry();
			t1 = ler_contador();
		}

		barreira();
		amostras[i] = t1 - t0;
		ciclos_medidos[i] = c1 - c0;
	}

	uint64_t f_depois = freq_nucleo_hz(nucleo);
	int nucleo_real = sched_getcpu();

	uint64_t *ordenadas = new uint64_t[n];

	std::copy(amostras, amostras + n, ordenadas);
	std::sort(ordenadas, ordenadas + n);

	double soma = 0.0;
	for (int i = 0; i < n; i++)
		soma += (double)ordenadas[i];

	double media    = soma / n;
	uint64_t minimo = ordenadas[0];
	uint64_t maximo = ordenadas[n - 1];
	double mediana  = (n % 2)
		? (double)ordenadas[n / 2]
		: ((double)ordenadas[n / 2 - 1] + (double)ordenadas[n / 2]) / 2.0;

	/* As mesmas quatro estatisticas sobre os ciclos medidos. */
	uint64_t *ord_pmu = new uint64_t[n];
	double media_pmu = 0.0, mediana_pmu = 0.0;
	uint64_t min_pmu = 0, max_pmu = 0;

	if (pmu_ok) {
		std::copy(ciclos_medidos, ciclos_medidos + n, ord_pmu);
		std::sort(ord_pmu, ord_pmu + n);

		double soma_pmu = 0.0;
		for (int i = 0; i < n; i++)
			soma_pmu += (double)ord_pmu[i];

		media_pmu   = soma_pmu / n;
		min_pmu     = ord_pmu[0];
		max_pmu     = ord_pmu[n - 1];
		mediana_pmu = (n % 2)
			? (double)ord_pmu[n / 2]
			: ((double)ord_pmu[n / 2 - 1] + (double)ord_pmu[n / 2]) / 2.0;
	}

	/* ================================================================ */
	/* AS DUAS CONVERSOES                                               */
	/* ================================================================ */
	/*
	 * A primeira e' exata. f_timer e' constante, gravada pelo firmware, e
	 * nao depende de nada que o sistema faca.
	 */
	double us_por_tick = f_timer ? 1e6 / (double)f_timer : 0.0;

	/*
	 * A segunda e' derivada, e so' vale sob duas condicoes: conhecer
	 * f_nucleo, e ele nao ter mudado durante o lote. Quando qualquer uma das
	 * duas falha, a coluna sai VAZIA no CSV. Vazio e' diferente de zero:
	 * "nao medi" nao e' "custou zero ciclos", e um campo em branco impede que
	 * alguem, meses depois, some uma coluna achando que o dado esta' la'.
	 */
	bool clock_estavel = (f_antes != 0) && (f_antes == f_depois);
	double ciclos_por_tick = clock_estavel
		? (double)f_antes / (double)f_timer : 0.0;

	double res_rel = mediana > 0 ? 100.0 / mediana : 0.0;
	double dispersao = mediana > 0
		? 100.0 * (double)(maximo - minimo) / mediana : 0.0;
	bool isolamento_completo = ok_afinidade && ok_fifo && ok_mlock &&
		strcmp(governor, "performance") == 0;

	/*
	 * Incerteza de quantizacao sobre a MEDIA.
	 *
	 * O contador incrementa a cada 18,52 ns, entao uma amostra isolada erra em
	 * ate' 1 tick. Mas a fase do contador no instante em que a medicao comeca e'
	 * essencialmente aleatoria, o que faz desse erro um ruido aproximadamente
	 * UNIFORME em [-1/2, +1/2] tick. Uniforme tem desvio padrao 1/sqrt(12), e a
	 * media de n amostras independentes divide o desvio por sqrt(n).
	 *
	 * Resultado: a media aguenta resolucao ruim muito melhor do que qualquer
	 * amostra isolada. Para o statemate com n=100, 1 tick vale 8,3 % de uma
	 * amostra e a media fica determinada dentro de ~0,24 %.
	 *
	 * O maximo NAO tem essa protecao. Ele e' uma amostra so', e nela um tick de
	 * arredondamento e uma execucao lenta sao indistinguiveis. Como o maximo e'
	 * a unica estatistica que responde a pergunta do WCET, e' ele que a
	 * bandeira abaixo qualifica.
	 */
	double incerteza_media = mediana > 0
		? 100.0 / (mediana * sqrt(12.0 * n)) : 0.0;
	double piso_pct = mediana > 0 ? 100.0 * (double)piso / mediana : 0.0;

	bool max_confiavel = clock_estavel && isolamento_completo && mediana >= 100.0;

	/* ================================================================ */
	/* CANAL 1: stdout, uma linha de CSV                                */
	/* ================================================================ */

	printf("%s,%d,%d,%d,%s,"
	       "%llu,%llu,%d,%d,"
	       "%llu,%.1f,%.1f,%llu,"
	       "%.4f,%.4f,%.4f,%.4f,",
	       BENCH_NAME, n, aquecimentos, nucleo_real, governor,
	       (unsigned long long)f_antes, (unsigned long long)f_timer,
	       clock_estavel ? 1 : 0, isolamento_completo ? 1 : 0,
	       (unsigned long long)minimo, mediana, media,
	       (unsigned long long)maximo,
	       minimo * us_por_tick, mediana * us_por_tick,
	       media * us_por_tick, maximo * us_por_tick);

	if (clock_estavel)
		printf("%.0f,%.0f,%.0f,",
		       media * ciclos_por_tick,
		       mediana * ciclos_por_tick,
		       maximo * ciclos_por_tick);
	else
		printf(",,,");        /* vazio, e nao zero */

	printf("%.2f,%.1f,%d,%llu,%.1f,%.3f,",
	       res_rel, dispersao, max_confiavel ? 1 : 0,
	       (unsigned long long)piso, piso_pct, incerteza_media);

	/* A razao entre o que a PMU contou e o que a etapa 7 calculava a partir dos
	 * ticks. Se der 1,00 a conversao derivada estava correta; qualquer desvio
	 * mede exatamente por quanto ela errava. */
	double ciclos_derivados = clock_estavel ? mediana * ciclos_por_tick : 0.0;
	double razao = (ciclos_derivados > 0.0 && mediana_pmu > 0.0)
		? mediana_pmu / ciclos_derivados : 0.0;

	if (pmu_ok)
		printf("1,%llu,%.1f,%.1f,%llu,%llu,%.4f,%.4f,%.1f\n",
		       (unsigned long long)min_pmu, mediana_pmu, media_pmu,
		       (unsigned long long)max_pmu, (unsigned long long)piso_pmu,
		       razao,
		       mediana_pmu > 0 ? 100.0 / mediana_pmu : 0.0,
		       mediana_pmu > 0
			       ? 100.0 * (double)(max_pmu - min_pmu) / mediana_pmu : 0.0);
	else
		printf("0,,,,,,,,\n");   /* vazio, e nao zero */
	fflush(stdout);

	/* ================================================================ */
	/* CANAL 2: stderr, o relatorio legivel                             */
	/* ================================================================ */

	fprintf(stderr, "etapa8 — benchmark \"%s\"\n", BENCH_NAME);
	fprintf(stderr, "  %d execucoes medidas, %d de aquecimento\n",
		n, aquecimentos);
	fprintf(stderr, "  CNTFRQ_EL0 = %.3f MHz (%.2f ns/tick)\n\n",
		f_timer / 1e6, ns_por_tick);

	fprintf(stderr, "AMBIENTE\n");
	fprintf(stderr, "  afinidade   nucleo %d          %s\n", nucleo,
		ok_afinidade ? "OK" : strerror(erro_afinidade));
	fprintf(stderr, "  nucleo real onde rodou: %d      %s\n", nucleo_real,
		nucleo_real == nucleo ? "" : "<<< NAO e' o pedido");
	fprintf(stderr, "  SCHED_FIFO  prioridade 80     %s\n",
		ok_fifo ? "OK" : strerror(erro_fifo));
	fprintf(stderr, "  mlockall    CURRENT|FUTURE    %s\n",
		ok_mlock ? "OK" : strerror(erro_mlock));
	fprintf(stderr, "  governor    %-16s %s\n", governor,
		strcmp(governor, "performance") == 0 ? "OK" : "<<< nao e' performance");
	fprintf(stderr, "  clock       %.0f MHz antes, %.0f MHz depois   %s\n",
		f_antes / 1e6, f_depois / 1e6,
		clock_estavel ? "estavel" : "<<< MUDOU");
	fprintf(stderr, "  PMCCNTR_EL0 %s\n",
		pmu_ok ? "acessivel e contando"
		       : "<<< INDISPONIVEL (modulo de kernel nao carregado)");

	if (!ok_fifo || !ok_mlock)
		fprintf(stderr,
			"\n  >>> Isolamento PARCIAL. Rode com sudo para o lote completo.\n");
	if (strcmp(governor, "performance") != 0)
		fprintf(stderr,
			"\n  >>> Para travar o clock:\n"
			"      echo performance | sudo tee "
			"/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor\n");
	fprintf(stderr, "\n");

	fprintf(stderr, "AMOSTRAS\n");
	for (int i = 0; i < n && i < 20; i++)
		fprintf(stderr, "  execucao %2d   %10llu ticks   %10.4f us\n",
			i + 1, (unsigned long long)amostras[i],
			amostras[i] * us_por_tick);
	if (n > 20)
		fprintf(stderr, "  ... (%d amostras restantes omitidas)\n", n - 20);
	fprintf(stderr, "\n");

	/* As tres colunas lado a lado, de proposito. A conversao fica auditavel:
	 * quem ler o relatorio ve' o dado bruto, o dado exato e o dado derivado, e
	 * pode refazer a conta. Reportar so' os ciclos esconderia que eles nao
	 * foram medidos, e sim calculados a partir de uma frequencia lida do
	 * sysfs. */
	fprintf(stderr, "RESUMO           ticks          us       ciclos (derivados)\n");
	fprintf(stderr, "  minimo   %10llu  %10.4f  %s\n",
		(unsigned long long)minimo, minimo * us_por_tick,
		clock_estavel ? "" : "(indisponivel)");
	fprintf(stderr, "  mediana  %10.1f  %10.4f  ", mediana,
		mediana * us_por_tick);
	if (clock_estavel)
		fprintf(stderr, "%12.0f\n", mediana * ciclos_por_tick);
	else
		fprintf(stderr, "(indisponivel)\n");
	fprintf(stderr, "  MEDIA    %10.1f  %10.4f  ", media, media * us_por_tick);
	if (clock_estavel)
		fprintf(stderr, "%12.0f   <- \"WCET observado\" do paper\n",
			media * ciclos_por_tick);
	else
		fprintf(stderr, "(indisponivel)\n");
	fprintf(stderr, "  maximo   %10llu  %10.4f  ",
		(unsigned long long)maximo, maximo * us_por_tick);
	if (clock_estavel)
		fprintf(stderr, "%12.0f   <- proxy de pior caso\n",
			maximo * ciclos_por_tick);
	else
		fprintf(stderr, "(indisponivel)\n");
	fprintf(stderr, "\n");

	/* ---- o confronto entre os dois instrumentos ---- */
	if (pmu_ok) {
		fprintf(stderr,
			"CICLOS MEDIDOS (PMCCNTR_EL0)   contra   CICLOS DERIVADOS (etapa 7)\n");
		fprintf(stderr, "  minimo   %12llu\n", (unsigned long long)min_pmu);
		fprintf(stderr, "  mediana  %12.1f", mediana_pmu);
		if (clock_estavel)
			fprintf(stderr, "        derivado %12.0f     razao %.4f\n",
				ciclos_derivados,
				ciclos_derivados > 0 ? mediana_pmu / ciclos_derivados : 0.0);
		else
			fprintf(stderr, "        derivado indisponivel\n");
		fprintf(stderr, "  MEDIA    %12.1f\n", media_pmu);
		fprintf(stderr, "  maximo   %12llu\n", (unsigned long long)max_pmu);
		fprintf(stderr, "  piso do instrumento  %llu ciclos = %.2f %% da mediana\n",
			(unsigned long long)piso_pmu,
			mediana_pmu > 0 ? 100.0 * (double)piso_pmu / mediana_pmu : 0.0);
		fprintf(stderr,
			"  resolucao  %.4f %% por amostra   (era %.2f %% com o generic timer,\n"
			"             ou seja %.0fx melhor)\n",
			mediana_pmu > 0 ? 100.0 / mediana_pmu : 0.0, res_rel,
			mediana_pmu > 0 && res_rel > 0
				? res_rel / (100.0 / mediana_pmu) : 0.0);
		fprintf(stderr, "  dispersao  %.1f %%   (era %.1f %%)\n",
			mediana_pmu > 0
				? 100.0 * (double)(max_pmu - min_pmu) / mediana_pmu : 0.0,
			dispersao);

		/* A razao e' o veredito sobre a etapa 7 inteira. */
		if (clock_estavel && ciclos_derivados > 0.0) {
			double r = mediana_pmu / ciclos_derivados;

			if (r > 0.98 && r < 1.02)
				fprintf(stderr,
					"\n  A conversao derivada da etapa 7 CONFERE (razao %.4f).\n"
					"  ticks x f_nucleo / f_timer recuperava o numero certo.\n", r);
			else
				fprintf(stderr,
					"\n  A conversao derivada da etapa 7 ERRAVA %.1f %% "
					"(razao %.4f).\n"
					"  A frequencia lida do sysfs nao descreve o que o nucleo\n"
					"  realmente fez durante a medicao.\n",
					(r - 1.0) * 100.0, r);
		}
		fprintf(stderr, "\n");
	}

	fprintf(stderr, "CONVERSAO\n");
	fprintf(stderr, "  ticks -> us       exata:    /%.3f MHz\n", f_timer / 1e6);
	if (clock_estavel)
		fprintf(stderr,
			"  ticks -> ciclos   DERIVADA: x %.4f  (= %.0f MHz / %.0f MHz)\n"
			"                    supoe clock parado no lote inteiro; conferido\n"
			"                    lendo scaling_cur_freq antes e depois.\n",
			ciclos_por_tick, f_antes / 1e6, f_timer / 1e6);
	else
		fprintf(stderr,
			"  ticks -> ciclos   RECUSADA: o clock do nucleo mudou durante o\n"
			"                    lote (%.0f -> %.0f MHz). Converter aqui daria\n"
			"                    um numero sem significado.\n",
			f_antes / 1e6, f_depois / 1e6);
	fprintf(stderr, "\n");

	fprintf(stderr, "DIAGNOSTICO\n");
	fprintf(stderr, "  dispersao (max-min)/mediana   %6.1f %%\n", dispersao);
	fprintf(stderr, "  media/mediana                 %6.3f\n",
		mediana > 0 ? media / mediana : 0.0);
	fprintf(stderr, "  resolucao relativa (1 tick)   %6.2f %%  em UMA amostra\n",
		res_rel);
	fprintf(stderr, "  incerteza da media (n=%d)     %6.3f %%  a quantizacao "
			"se cancela na media\n", n, incerteza_media);
	fprintf(stderr, "  piso do instrumento           %6llu ticks = %.1f %% da "
			"mediana\n", (unsigned long long)piso, piso_pct);
	fprintf(stderr, "\n");

	/* A distincao que importa, e que o rotulo antigo ("utilizavel") escondia:
	 * resolucao ruim estraga o MAXIMO e poupa a MEDIA. Dizer so' "nao
	 * utilizavel" sugeria que a medicao inteira era lixo, o que e' falso. */
	if (mediana < 100.0) {
		fprintf(stderr,
			"  RESOLUCAO BAIXA (1 tick = %.2f %% da mediana)\n"
			"    MEDIA     confiavel dentro de ~%.3f %%. A fase do contador no\n"
			"              inicio de cada medicao e' aleatoria, entao o erro de\n"
			"              arredondamento e' ~uniforme e se cancela em %d\n"
			"              amostras. O valor em microssegundos vale.\n"
			"    MAXIMO    NAO confiavel. E' uma amostra so', e nela um tick de\n"
			"              arredondamento e uma execucao lenta ficam\n"
			"              indistinguiveis. Como o maximo e' a estatistica que\n"
			"              responde a pergunta do WCET, e' esta a perda que\n"
			"              importa.\n"
			"    DISPERSAO majoritariamente quantizacao, nao variabilidade real.\n",
			res_rel, incerteza_media, n);
	} else if (mediana < 1000.0) {
		fprintf(stderr,
			"  ATENCAO: mediana de %.1f ticks da' so' %.2f %% de resolucao por "
			"amostra. Use o maximo com ressalva.\n", mediana, res_rel);
	}

	if (piso_pct > 5.0)
		fprintf(stderr,
			"  VIES SISTEMATICO: o piso do instrumento e' %.1f %% da mediana.\n"
			"    Esse custo esta' DENTRO de todas as amostras e infla o\n"
			"    resultado para cima. Nao e' ruido: repetir mais nao o remove.\n"
			"    O programa NAO subtrai, porque supor sobreposicao nula entre o\n"
			"    isb e o benchmark seria errado num nucleo fora de ordem. Trate\n"
			"    o valor reportado como um limite superior do custo real.\n",
			piso_pct);

	fprintf(stderr, "\n  max_confiavel = %d  (clock estavel, isolamento "
			"completo, mediana >= 100 ticks)\n", max_confiavel ? 1 : 0);

	/* O contador de ciclos e' POR NUCLEO e nucleos diferentes nao estao
	 * sincronizados. Uma migracao no meio do lote corrompe todas as diferencas,
	 * possivelmente produzindo valores negativos que aparecem como numeros
	 * gigantes por serem sem sinal. */
	if (pmu_ok && nucleo_real != nucleo)
		fprintf(stderr,
			"\n  ALERTA: a thread terminou no nucleo %d e nao no %d pedido.\n"
			"    O PMCCNTR_EL0 e' por nucleo e os valores nao sao comparaveis\n"
			"    entre nucleos. Descarte este lote.\n", nucleo_real, nucleo);

	delete[] ord_pmu;
	delete[] ciclos_medidos;
	delete[] ordenadas;
	delete[] amostras;
	return 0;
}
