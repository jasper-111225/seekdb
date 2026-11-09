CREATE TABLE docs (
    id BIGINT PRIMARY KEY,
    category VARCHAR(32),
    content VARCHAR(1024),
    embedding VECTOR(4)
);
INSERT INTO docs VALUES
    (1, 'database', '事务与恢复', '[1, 0, 0, 0]'),
    (2, 'database', '向量索引',   '[0, 1, 0, 0]'),
    (3, 'travel',   '旅行攻略',   '[0, 0, 1, 0]');
SELECT id, content, L2_DISTANCE(embedding, '[0.9, 0.1, 0, 0]') AS distance FROM docs WHERE category = 'database' ORDER BY distance LIMIT 2;
SELECT * FROM docs;
SELECT id FROM docs ORDER BY id DESC;
BEGIN;
UPDATE docs SET embedding = '[0.8, 0.2, 0, 0]' WHERE id = 1;
DELETE FROM docs WHERE id = 2;
SELECT id FROM docs ORDER BY id;
ROLLBACK;
SELECT id FROM docs ORDER BY id;
INSERT INTO docs VALUES (2, 'database', '向量索引', '[0, 1, 0, 0]');
SELECT id FROM docs ORDER BY id;
