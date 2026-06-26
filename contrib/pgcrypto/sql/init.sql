--
-- init pgcrypto
--
-- 初始化pgcrypto
--

CREATE EXTENSION pgcrypto;

-- check error handling
--
-- 检查错误处理
select gen_salt('foo');
select digest('foo', 'foo');
select hmac('foo', 'foo', 'foo');
select encrypt('foo', 'foo', 'foo');
