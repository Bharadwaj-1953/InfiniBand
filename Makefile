CC=mpicc
TARGET=dht_count
SCRIPT=$(TARGET).c

all: compile run

compile:
	$(CC) -o $(TARGET) $(SCRIPT) -libverbs

run:
	@echo "Enter the test file (e.g., test1.txt):"
	@read TEST_FILE; \
	echo "Enter the query file (e.g., query.txt):"; \
	read QUERY_FILE; \
	echo "Running $(TARGET) with 8 processes (2 x 4) on files $$TEST_FILE and $$QUERY_FILE"; \
	mpirun --mca btl_tcp_if_include ibp216s0f0 -np 8 --hostfile hosts ./$(TARGET) $$TEST_FILE $$QUERY_FILE

