#!/bin/sh
# Fetches the input data of the Voyager 2 simulation from NASA JPL Horizons
# (https://ssd.jpl.nasa.gov/horizons/, public, no account). The files are already
# in data/; run this only to refresh them or to see where they come from.
#
# Each file is a Horizons vector table: position and velocity in km and km/s,
# ICRF axes, time TDB. Cruise files are centred on the solar system's centre of
# mass (500@0), flyby files on the planet (500@599 and so on).
set -e
cd "$(dirname "$0")/data"

hz() {  # hz <body> <centre> <start> <stop> <step> <file>
  curl -s -m 180 -A "curl" -G "https://ssd.jpl.nasa.gov/api/horizons.api" \
    --data-urlencode "format=text" --data-urlencode "COMMAND='$1'" --data-urlencode "OBJ_DATA='NO'" \
    --data-urlencode "MAKE_EPHEM='YES'" --data-urlencode "EPHEM_TYPE='VECTORS'" --data-urlencode "CENTER='$2'" \
    --data-urlencode "REF_PLANE='FRAME'" --data-urlencode "START_TIME='$3'" --data-urlencode "STOP_TIME='$4'" \
    --data-urlencode "STEP_SIZE='$5'" --data-urlencode "VEC_TABLE='2'" --data-urlencode "OUT_UNITS='KM-S'" \
    --data-urlencode "CSV_FORMAT='YES'" -o "$6"
  n=$(sed -n '/\$\$SOE/,/\$\$EOE/p' "$6" | grep -c "A.D." || true)
  printf "%-16s %5s rows\n" "$6" "$n"
  [ "$n" -gt 1 ] || { echo "no data in $6" >&2; exit 1; }
}

# ---- cruise: Sun and the four giant-planet systems, 1977 to 2026 ----
for b in 10 5 6 7 8; do hz $b 500@0 1977-06-01 2026-12-20 '20 d' bary_$b.txt; done
# the inner planets while Voyager 2 is still near them (Mercury, Venus, Earth-Moon, Mars)
for b in 1 2 3 4; do hz $b 500@0 1977-08-01 1979-07-25 '2 d' bary_$b.txt; done
# Voyager 2 itself, one file per cruise leg
hz -32 500@0 1977-09-01 1979-07-05 '5 d'  v2_leg1.txt
hz -32 500@0 1979-07-14 1981-08-22 '5 d'  v2_leg3.txt
hz -32 500@0 1981-08-30 1986-01-21 '10 d' v2_leg5.txt
hz -32 500@0 1986-01-28 1989-08-22 '10 d' v2_leg7.txt
hz -32 500@0 1989-08-28 2026-09-28 '60 d' v2_leg9.txt

# ---- flybys: everything as seen from the planet's centre ----
hz -32 500@599 1979-07-05 1979-07-14 '10 m' jup_v2.txt
for b in 10 6; do hz $b 500@599 1979-07-03 1979-07-16 '6 h' jup_$b.txt; done
for b in 501 502 503 504; do hz $b 500@599 1979-07-04 1979-07-15 '20 m' jup_$b.txt; done

hz -32 500@699 1981-08-22 1981-08-30 '10 m' sat_v2.txt
for b in 10 5; do hz $b 500@699 1981-08-20 1981-09-01 '6 h' sat_$b.txt; done
for b in 603 604 605 606; do hz $b 500@699 1981-08-21 1981-08-31 '20 m' sat_$b.txt; done

hz -32 500@799 1986-01-21 1986-01-28 '10 m' ura_v2.txt
hz 10 500@799 1986-01-19 1986-01-30 '6 h' ura_10.txt
for b in 701 702 703 704 705; do hz $b 500@799 1986-01-20 1986-01-29 '20 m' ura_$b.txt; done

hz -32 500@899 1989-08-22 1989-08-28 '10 m' nep_v2.txt
hz 10 500@899 1989-08-20 1989-08-30 '6 h' nep_10.txt
hz 801 500@899 1989-08-21 1989-08-29 '20 m' nep_801.txt
