---
--- Testing cube output in scientific notation.  This was put into separate
--
--- 以科学记数法测试立方体输出。  这个被单独放入
--- test, because has platform-depending output.
--
--- 测试，因为具有依赖于平台的输出。
---

SELECT '1e27'::cube AS cube;
SELECT '-1e27'::cube AS cube;
SELECT '1.0e27'::cube AS cube;
SELECT '-1.0e27'::cube AS cube;
SELECT '1e+27'::cube AS cube;
SELECT '-1e+27'::cube AS cube;
SELECT '1.0e+27'::cube AS cube;
SELECT '-1.0e+27'::cube AS cube;
SELECT '1e-7'::cube AS cube;
SELECT '-1e-7'::cube AS cube;
SELECT '1.0e-7'::cube AS cube;
SELECT '-1.0e-7'::cube AS cube;
SELECT '1e-300'::cube AS cube;
SELECT '-1e-300'::cube AS cube;
SELECT '1234567890123456'::cube AS cube;
SELECT '+1234567890123456'::cube AS cube;
SELECT '-1234567890123456'::cube AS cube;
