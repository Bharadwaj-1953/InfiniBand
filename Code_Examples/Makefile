all:
	gcc -g ib-nocma-client.c -o ib-nocma-client -libverbs
	gcc -g ib-nocma-server.c -o ib-nocma-server -libverbs
	gcc -g ib-nocma.c -o ib-nocma -libverbs
	gcc -g ib-cma-client.c -o ib-cma-client -libverbs -lrdmacm
	gcc -g ib-cma-server.c -o ib-cma-server -libverbs -lrdmacm

dht:
	mpicc dht_count.c -o dht_count -libverbs -lrdmacm
#	mpirun --mca btl_tcp_if_include ibp216s0f0 -np 8 --hostfile ./hosts.txt ./dht_count
#	The contents of the ./hosts.txt file are:
#	10.10.15.51 slots=4  
#	10.10.15.52 slots=4
#	mpirun --mca btl_tcp_if_include ibp216s0f0 -np 8 --hostfile ./hosts.txt ./dht_count query.txt test1.txt 
#	mpirun --mca btl_tcp_if_include ibp216s0f0 -np 8 --hostfile ./hosts.txt ./dht_count query.txt test2.txt 

mpi-ib:
	mpicc -o mpi-ib ./mpi-ib-cma.c  -libverbs -lrdmacm
#	mpirun --mca btl_tcp_if_include ibp216s0f0--mca btl_tcp_if_include ibp216s0f0 -np 2 -host ns02-ib0,ns03-ib0 ../hello_mpi
#	mpirun --mca btl_tcp_if_include ibp216s0f0--mca btl_tcp_if_include ibp216s0f0 -np 2 -host ns02-ib0,ns03-ib0 ./mpi-ib 18700

clean: 
	rm -f ib-nocma-client ib-nocma-server ib-nocma mpi-ib *.bak *.c~
	rm -f ib-cma-client ib-cma-server *.bak *.c~
