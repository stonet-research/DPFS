#!/bin/bash

echo "Welcome to the dpu-virtio-fs workload runner! :)"
echo "Please run this script with numactl to bind all the workloads to the NUMA node on which the device is located"
echo "Furthermore please make sure that any resource hogs have been removed from the system"

if [ -z $MNT ]; then
	echo "You must set the MNT env variable to where you want to run the workloads!"
	exit 1
fi

if perf 2>&1 | grep "WARNING: perf not found for kernel"; then
	exit 1
fi

DEV=$1
if [[ $DEV != "NFS" && $DEV !=  "VNFS" && $DEV != "nulldev" ]]; then
	echo "You must supply this script with one of the following parameters: NFS, VNFS or nulldev"
	exit 1
fi

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
cd $SCRIPT_DIR

TIMESTAMP=$(date +"%Y-%m-%d_%T")
HOST=$(uname -n)
COMMIT=$(git rev-parse --short HEAD)
OUT=./output/${COMMIT}_${HOST}_${TIMESTAMP}

echo "Setting CPU/DMA latency to 0. A sudo prompt will now come up"
gcc setcpulatency.c -o setcpulatency
sudo ./setcpulatency 0 & 

echo "Setting /proc/sys/kernel/perf_event_paranoid to -1 for perf."
sudo sh -c "echo -1 > /proc/sys/kernel/perf_event_paranoid"

TIME=$(python3 -c 'print(round((2*10*8*70 + 2*5*70 + 610*3 + 610*3)/60/60, 2))')
echo "The output will be stored under $OUT"

echo "STARTING in 10 seconds! quit the system now to reduce variability!"
echo "This run.sh will take $TIME hours. Only log back in after that amount of time!"
sleep 10
echo "START"

mkdir -p $OUT

echo "Running: fio latency, IOPS and throughput singlecore experiments"
BS_LIST=()
# NFS supports up to 1m
if [[ $DEV == "NFS" ]]; then
	BS_LIST=("1" "4k" "8k" "16k" "32k" "64k" "128k" "256k" "512k" "1m")
elif [[ $DEV == "VNFS" || $DEV == "nulldev" ]]; then
	BS_LIST=("1" "4k" "8k" "16k" "32k" "64k" "128k")
fi
for RW in "randread" "randwrite"; do
	for BS in "${BS_LIST[@]}"; do
		for IODEPTH in 1 2 4 8 16 32 64 128; do
			for P in 1; do
				echo fio RW=$RW BS=$BS IODEPTH=$IODEPTH P=$P
				RW=$RW BS=$BS IODEPTH=$IODEPTH P=$P ./workloads/fio.sh > $OUT/fio_${RW}_${BS}_${IODEPTH}_${P}.out
			done
		done
	done
done

# Multicore experiment
# Thread count 1, 2, 4, 8, 16
# BS fixed (fastest for VNFS)
# QD fixed (fastest aka 128)
echo "Running: fio IOPS multicore experiment"
for RW in "randread" "randwrite"; do
	for BS in "32k"; do
		for IODEPTH in 128; do
			for P in 1 2 4 8 16; do
				echo fio RW=$RW BS=$BS IODEPTH=$IODEPTH P=$P
				RW=$RW BS=$BS IODEPTH=$IODEPTH P=$P ./workloads/fio.sh > $OUT/fio_${RW}_${BS}_${IODEPTH}_${P}.out
			done
		done
	done
done

# Single operation latency
echo "Running: stat (getattr) latency"
gcc ./workloads/lat/lat_stat.c -O3 -o ./workloads/lat/lat_stat
./workloads/lat/lat_stat $MNT/test 100000 1 > $OUT/lat_stat.out
echo "Running: statfs latency"
gcc ./workloads/lat/lat_statfs.c -O3 -o ./workloads/lat/lat_statfs
./workloads/lat/lat_statfs $MNT 100000 1 > $OUT/lat_statfs.out

sudo pkill setcpulatency
echo "Reset CPU/DMA latency to default value"

RUNTIME="600"
REPS=3
echo "Running: perf CPU cycle analysis for ${RUNTIME}s seconds without a load (baseline)"
for i in $(seq 1 $REPS); do
	# We are doing -a (system wide profiling) to take the RX path into account that partially doesn't get attributed to the process.
	# add 10 seconds to the runtime to account for the startup
	perf stat -a -x "," --delay 10000 --all-kernel -- sleep 610 2> $OUT/cpu_baseline_perf_${i}.out
done

echo "Running: perf CPU cycle analysis for ${RUNTIME}s seconds with a fio stress load"
for i in $(seq 1 $REPS); do
	# Our fio by default warms up for 10s, so just runtime=300s
	perf stat -a -x "," --delay 10000 --all-kernel -- env RW=randrw BS=4k IODEPTH=128 P=1 RUNTIME=${RUNTIME}s ./workloads/fio.sh 1> $OUT/cpu_load_fio_${i}.out 2> $OUT/cpu_load_perf_${i}.out
done

echo "DONE"

echo If the experiment was successful and the results are verified, please move them to the results folder of the implementation that was used
