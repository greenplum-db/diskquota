DROP TABLE IF EXISTS "t3";

CREATE TABLE "t3" (
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
DISTRIBUTED RANDOMLY;