#!/bin/bash

echo "Running synthetic experiments"

echo "Running: fio IOPS and throughput experiments"
BS_LIST=()
# NFS supports up to 1m
if [[ $DEV == "NFS" ]]; then
	BS_LIST=("1" "4k" "8k" "16k" "32k" "64k" "128k" "256k" "512k" "1m")
elif [[ $DEV == "VNFS" || $DEV == "nulldev" ]]; then
	BS_LIST=("1" "4k" "8k" "16k" "32k" "64k" "128k")
fi
QD_LIST=()
# The nulldev sees increases in iops until QD 256
if [[ $DEV == "nulldev" ]]; then
	QD_LIST=("1" "2" "4" "8" "16" "32" "64" "128" "256" "512")
elif [[ $DEV == "NFS" || $DEV == "VNFS" ]]; then
	QD_LIST=("1" "2" "4" "8" "16" "32" "64" "128")
fi
for RW in "randread" "randwrite"; do
	for BS in "${BS_LIST[@]}"; do
		for IODEPTH in "${QD_LIST[@]}"; do
			for P in 2; do
				echo fio RW=$RW BS=$BS IODEPTH=$IODEPTH P=$P
				RW=$RW BS=$BS IODEPTH=$IODEPTH P=$P ./workloads/fio.sh > $OUT/fio_${RW}_${BS}_${IODEPTH}_${P}.out
			done
		done
	done
done

sudo ./setcpulatency 0 &
sleep 5
echo "Running: fio latency experiments"
for RW in "randread" "randwrite"; do
	for BS in "${BS_LIST[@]}"; do
		for IODEPTH in 1; do
			for P in 1; do
				echo fio RW=$RW BS=$BS IODEPTH=$IODEPTH P=$P
				RW=$RW BS=$BS IODEPTH=$IODEPTH P=$P ./workloads/fio.sh > $OUT/fio_${RW}_${BS}_${IODEPTH}_${P}.out
			done
		done
	done
done
sudo pkill setcpulatency
sleep 5

# Multicore experiment
# Thread count 1, 2, 4, 8, 16
# BS fixed (fastest for VNFS)
# QD fixed (fastest aka 128)
echo "Running: fio IOPS multicore experiment"
for RW in "randread" "randwrite"; do
	for BS in "4k" "32k"; do
		for IODEPTH in 128; do
			for P in 1 2 4 8; do
				echo fio-multifile RW=$RW BS=$BS IODEPTH=$IODEPTH P=$P
				RW=$RW BS=$BS IODEPTH=$IODEPTH P=$P ./workloads/fio-multifile.sh > $OUT/fio_${RW}_${BS}_${IODEPTH}_${P}.out
			done
		done
	done
done

sleep 10
