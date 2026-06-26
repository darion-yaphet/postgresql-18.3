SET md5_password_warnings = off;
LOAD 'passwordcheck';

CREATE USER regress_passwordcheck_user1;

-- ok
ALTER USER regress_passwordcheck_user1 PASSWORD 'a_nice_long_password';

-- error: too short
--
-- 错误：太短
ALTER USER regress_passwordcheck_user1 PASSWORD 'tooshrt';

-- ok
SET passwordcheck.min_password_length = 6;
ALTER USER regress_passwordcheck_user1 PASSWORD 'v_shrt';

-- error: contains user name
--
-- 错误：包含用户名
ALTER USER regress_passwordcheck_user1 PASSWORD 'xyzregress_passwordcheck_user1';

-- error: contains only letters
--
-- 错误：仅包含字母
ALTER USER regress_passwordcheck_user1 PASSWORD 'alessnicelongpassword';

-- encrypted ok (password is "secret")
--
-- 加密好的（密码是“秘密”）
ALTER USER regress_passwordcheck_user1 PASSWORD 'md592350e12ac34e52dd598f90893bb3ae7';

-- error: password is user name
--
-- 错误：密码是用户名
ALTER USER regress_passwordcheck_user1 PASSWORD 'md507a112732ed9f2087fa90b192d44e358';

DROP USER regress_passwordcheck_user1;
