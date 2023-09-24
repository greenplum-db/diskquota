DROP TABLE IF EXISTS "t1";

CREATE TABLE "t1" (
  id SERIAL,
  part_1 integer NULL,
  part_2 integer NULL,
  part_3 integer NULL,
  address text default NULL,
  email text default NULL,
  phone text default NULL,
  name text default NULL,
  postalZip text default NULL,
  region text default NULL,
  country text default NULL,
  list text default NULL,
  alphanumeric text,
  currency text default NULL,
  numberrange integer NULL,
  content TEXT default NULL
)
DISTRIBUTED BY (id)
PARTITION BY RANGE (part_1)

SUBPARTITION BY RANGE (part_2)
SUBPARTITION TEMPLATE (
    START (1) END (5) EVERY (1),
    DEFAULT SUBPARTITION bad_part2)

SUBPARTITION BY RANGE (part_3)
SUBPARTITION TEMPLATE (
    START (1) END (5) EVERY (1),
    DEFAULT SUBPARTITION bad_part3)

( START (1) END (5) EVERY (1),
  DEFAULT PARTITION bad_part1);
