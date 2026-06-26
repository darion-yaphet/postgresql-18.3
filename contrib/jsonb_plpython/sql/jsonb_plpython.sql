CREATE EXTENSION jsonb_plpython3u CASCADE;

-- test jsonb -> python dict
--
-- 测试 jsonb -> python 字典
CREATE FUNCTION test1(val jsonb) RETURNS int
LANGUAGE plpython3u
TRANSFORM FOR TYPE jsonb
AS $$
assert isinstance(val, dict)
assert(val == {'a': 1, 'c': 'NULL'})
return len(val)
$$;

SELECT test1('{"a": 1, "c": "NULL"}'::jsonb);

-- test jsonb -> python dict
--
-- 测试 jsonb -> python 字典
-- complex dict with dicts as value
--
-- 以 dicts 作为值的复杂字典
CREATE FUNCTION test1complex(val jsonb) RETURNS int
LANGUAGE plpython3u
TRANSFORM FOR TYPE jsonb
AS $$
assert isinstance(val, dict)
assert(val == {"d": {"d": 1}})
return len(val)
$$;

SELECT test1complex('{"d": {"d": 1}}'::jsonb);


-- test jsonb[] -> python dict
--
-- 测试 jsonb[] -> python 字典
-- dict with array as value
--
-- 以数组作为值的字典
CREATE FUNCTION test1arr(val jsonb) RETURNS int
LANGUAGE plpython3u
TRANSFORM FOR TYPE jsonb
AS $$
assert isinstance(val, dict)
assert(val == {"d": [12, 1]})
return len(val)
$$;

SELECT test1arr('{"d":[12, 1]}'::jsonb);

-- test jsonb[] -> python list
--
-- 测试 jsonb[] -> python 列表
-- simple list
--
-- 简单列表
CREATE FUNCTION test2arr(val jsonb) RETURNS int
LANGUAGE plpython3u
TRANSFORM FOR TYPE jsonb
AS $$
assert isinstance(val, list)
assert(val == [12, 1])
return len(val)
$$;

SELECT test2arr('[12, 1]'::jsonb);

-- test jsonb[] -> python list
--
-- 测试 jsonb[] -> python 列表
-- array of dicts
--
-- 字典数组
CREATE FUNCTION test3arr(val jsonb) RETURNS int
LANGUAGE plpython3u
TRANSFORM FOR TYPE jsonb
AS $$
assert isinstance(val, list)
assert(val == [{"a": 1,"b": 2}, {"c": 3,"d": 4}])
return len(val)
$$;

SELECT test3arr('[{"a": 1, "b": 2}, {"c": 3,"d": 4}]'::jsonb);

-- test jsonb int -> python int
--
-- 测试 jsonb int -> python int
CREATE FUNCTION test1int(val jsonb) RETURNS int
LANGUAGE plpython3u
TRANSFORM FOR TYPE jsonb
AS $$
assert(val == 1)
return val
$$;

SELECT test1int('1'::jsonb);

-- test jsonb string -> python string
--
-- 测试 jsonb 字符串 -> python 字符串
CREATE FUNCTION test1string(val jsonb) RETURNS text
LANGUAGE plpython3u
TRANSFORM FOR TYPE jsonb
AS $$
assert(val == "a")
return val
$$;

SELECT test1string('"a"'::jsonb);

-- test jsonb null -> python None
--
-- 测试 jsonb null -> python None
CREATE FUNCTION test1null(val jsonb) RETURNS int
LANGUAGE plpython3u
TRANSFORM FOR TYPE jsonb
AS $$
assert(val == None)
return 1
$$;

SELECT test1null('null'::jsonb);

-- test python -> jsonb
--
-- 测试 python -> jsonb
CREATE FUNCTION roundtrip(val jsonb) RETURNS jsonb
LANGUAGE plpython3u
TRANSFORM FOR TYPE jsonb
as $$
return val
$$;

SELECT roundtrip('null'::jsonb);
SELECT roundtrip('1'::jsonb);
SELECT roundtrip('1234567890.0987654321'::jsonb);
SELECT roundtrip('-1234567890.0987654321'::jsonb);
SELECT roundtrip('true'::jsonb);
SELECT roundtrip('"string"'::jsonb);

SELECT roundtrip('{"1": null}'::jsonb);
SELECT roundtrip('{"1": 1}'::jsonb);
SELECT roundtrip('{"1": true}'::jsonb);
SELECT roundtrip('{"1": "string"}'::jsonb);

SELECT roundtrip('[null]'::jsonb);
SELECT roundtrip('[1]'::jsonb);
SELECT roundtrip('[true]'::jsonb);
SELECT roundtrip('["string"]'::jsonb);
SELECT roundtrip('[null, 1]'::jsonb);
SELECT roundtrip('[1, true]'::jsonb);
SELECT roundtrip('[true, "string"]'::jsonb);
SELECT roundtrip('["string", "string2"]'::jsonb);

-- complex numbers -> jsonb
--
-- 复数 -> jsonb
CREATE FUNCTION testComplexNumbers() RETURNS jsonb
LANGUAGE plpython3u
TRANSFORM FOR TYPE jsonb
AS $$
x = 1 + 2j
return x
$$;

SELECT testComplexNumbers();

-- range -> jsonb
--
-- 范围 -> jsonb
CREATE FUNCTION testRange() RETURNS jsonb
LANGUAGE plpython3u
TRANSFORM FOR TYPE jsonb
AS $$
x = range(3)
return x
$$;

SELECT testRange();

-- 0xff -> jsonb
--
-- 0xff -> jsonb
CREATE FUNCTION testDecimal() RETURNS jsonb
LANGUAGE plpython3u
TRANSFORM FOR TYPE jsonb
AS $$
x = 0xff
return x
$$;

SELECT testDecimal();

-- tuple -> jsonb
--
-- 元组 -> jsonb
CREATE FUNCTION testTuple() RETURNS jsonb
LANGUAGE plpython3u
TRANSFORM FOR TYPE jsonb
AS $$
x = (1, 'String', None)
return x
$$;

SELECT testTuple();

-- interesting dict -> jsonb
--
-- 有趣的字典 -> jsonb
CREATE FUNCTION test_dict1() RETURNS jsonb
LANGUAGE plpython3u
TRANSFORM FOR TYPE jsonb
AS $$
x = {"a": 1, None: 2, 33: 3}
return x
$$;

SELECT test_dict1();
