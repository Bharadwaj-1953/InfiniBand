# Run RDMA client and server without cma

```

make 

# on the server, e.g., ns03
./ib-nocma-server <port>

# on the client
./ib-nocma-client ns03 <port>
```


The client RDMA writes into server memory with a small message.

# Run RDMA client and server with cma



make 

# configure IPoIB interface on two nodes ns02 and ns03.

```
   modprobe ib_ipoib
   ifconfig -a (to show all interfaces)
   ifconfig ibp216s0f0 10.10.16.51 netmask 255.255.255.0
   ifconfig ibp216s0f0 10.10.16.52 netmask 255.255.255.0
```


# Also need to update their hostnames
```
hostname ns02-ib0
hostname ns03-ib0
```

# Then it is ready to run the server first on ns03-ib0
```
./ib-cma-server <port>
```

# Next run the client on ns02-ib0
```
./ib-cma-client <ns03-ib0> <port>
```


The client RDMA writes into server memory with a small message.

# Run the mpi-ib-cma.c in which one MPI process is the client and another server.

# Need to use OpenMPI instead of MPICH because of library dependency issues.

```
[wkyu@ns03-ib0 cma]$ mpicc -o mpi-ib ./mpi-ib-cma.c   -libverbs -lrdmacm
[wkyu@ns03-ib0 cma]$ mpirun -np 2 -host ns02-ib0,ns03-ib0 ../hello_mpi
rank 0 hostname:  size 4
rank 1 hostname:  size 4
rank 0 hostname: ns02-ib0
rank 1 hostname: ns03-ib0
[wkyu@ns03-ib0 cma]$ mpirun -np 2 -host ns02-ib0,ns03-ib0 ./mpi-ib 18700
rank 0 line 543: closed sockets.
rank 1 line 573: closed sockets.
```


# You may need to add an option to select btl

```
[wkyu@ns03-ib0 cma]$ mpirun --mca btl_tcp_if_include ibp216s0f0 -np 2 -host ns02-ib0,ns03-ib0 ../hello_mpi
rank 0 hostname:  size 4
rank 1 hostname:  size 4
rank 0 hostname: ns02-ib0
rank 1 hostname: ns03-ib0
[wkyu@ns03-ib0 cma]$ mpirun --mca btl_tcp_if_include ibp216s0f0 -np 2 -host ns02-ib0,ns03-ib0 ./mpi-ib 18700
rank 0 line 543: closed sockets.
rank 1 line 573: closed sockets.
```



