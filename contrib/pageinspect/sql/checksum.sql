--
-- Verify correct calculation of checksums
--
-- 验证校验和的计算是否正确
--
-- Postgres' checksum algorithm produces different answers on little-endian
--
-- Postgres 的校验和算法在小端上产生不同的答案
-- and big-endian machines.  The results of this test also vary depending
--
-- 和大端机器。  该测试的结果也因情况而异
-- on the configured block size.  This test has several different expected
--
-- 根据配置的块大小。  这个测试有几个不同的预期
-- results files to handle the following possibilities:
--
-- 结果文件来处理以下可能性：
--
--	BLCKSZ	end	file
--
--	BLCKSZ 结束文件
--	8K	LE	checksum.out
--
--	8K LE 校验和.out
--	8K	BE	checksum_1.out
--
--	8K BE 校验和_1.out
--
-- In future we might provide additional expected-results files for other
--
-- 将来我们可能会为其他人提供额外的预期结果文件
-- block sizes, but there seems little point as long as so many other
--
-- 块大小，但似乎没有什么意义只要这么多其他
-- test scripts also show false failures for non-default block sizes.
--
-- 测试脚本还显示非默认块大小的错误失败。
--

-- This is to label the results files with blocksize:
--
-- 这是用块大小标记结果文件：
SHOW block_size;

SHOW block_size \gset

-- Apply page_checksum() to some different data patterns and block numbers
--
-- 将 page_checksum() 应用于一些不同的数据模式和块编号
SELECT blkno,
    page_checksum(decode(repeat('01', :block_size), 'hex'), blkno) AS checksum_01,
    page_checksum(decode(repeat('04', :block_size), 'hex'), blkno) AS checksum_04,
    page_checksum(decode(repeat('ff', :block_size), 'hex'), blkno) AS checksum_ff,
    page_checksum(decode(repeat('abcd', :block_size / 2), 'hex'), blkno) AS checksum_abcd,
    page_checksum(decode(repeat('e6d6', :block_size / 2), 'hex'), blkno) AS checksum_e6d6,
    page_checksum(decode(repeat('4a5e', :block_size / 2), 'hex'), blkno) AS checksum_4a5e
  FROM generate_series(0, 100, 50) AS a (blkno);
