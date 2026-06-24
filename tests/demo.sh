#!/bin/bash
# Demo run: starts server, runs client which auto-uploads,
# performs a CRUD sequence, then prints the resulting CSV.
set -e
cd "$(dirname "$0")/.."

# Fresh starting CSV
cat > /tmp/input.csv <<EOF
id,name,age,grade
1,Aanya Sharma,21,A
2,Rohan Verma,22,B
3,Ishaan Reddy,20,A
4,Priya Iyer,23,A
5,Kabir Singh,21,C
EOF
cp /tmp/input.csv data/students.csv

nohup timeout 8 ./bin/student_server --ws-port 9120 --http-port 8200 \
    --csv data/students.csv > /tmp/demo_srv.log 2>&1 < /dev/null &
sleep 0.8

# Client: feed it a CRUD script via stdin. Since we're in data/ dir,
# auto-upload will find data/students.csv automatically.
./bin/student_client --port 9120 <<'CLIENT' > /tmp/demo_client.log 2>&1
create 100 "Aanya New" 22 A
update 100 "Aanya Updated" 23 S
delete 3
sort name asc
stats
quit
CLIENT

sleep 0.5
echo "=== CLIENT OUTPUT ==="
cat /tmp/demo_client.log
echo
echo "=== FINAL CSV (output) ==="
cat data/students.csv
echo
echo "=== SERVER LOG ==="
tail -15 /tmp/demo_srv.log
# Restore canonical sample
cat > data/students.csv <<EOF
id,name,age,grade
1,Aanya Sharma,21,A
2,Rohan Verma,22,B
3,Ishaan Reddy,20,A
4,Priya Iyer,23,A
5,Kabir Singh,21,C
6,Ananya Kapoor,22,B
7,Arjun Mehta,24,A
8,Diya Nair,20,B
9,Vivaan Joshi,21,A
10,Saanvi Rao,22,C
11,Aditya Bose,23,B
12,Myra Pillai,20,A
13,Reyansh Khan,22,B
14,Anika Desai,21,A
15,Krishna Patel,23,B
EOF
