-- SQL commands to view all tables in the trading schema with their attributes and types
-- This excludes the results schema as requested

-- 1. Show all tables in the trading schema
SELECT 
    table_name,
    table_type
FROM information_schema.tables 
WHERE table_schema = 'trading'
ORDER BY table_name;

-- 2. Detailed view of all columns in trading schema tables with full attribute information
SELECT 
    t.table_name,
    c.column_name,
    c.ordinal_position,
    c.column_default,
    c.is_nullable,
    c.data_type,
    c.character_maximum_length,
    c.numeric_precision,
    c.numeric_scale,
    c.datetime_precision,
    c.udt_name,
    CASE 
        WHEN c.column_name IN (
            SELECT kcu.column_name 
            FROM information_schema.table_constraints tc
            JOIN information_schema.key_column_usage kcu 
                ON tc.constraint_name = kcu.constraint_name
                AND tc.table_schema = kcu.table_schema
            WHERE tc.constraint_type = 'PRIMARY KEY'
                AND tc.table_schema = 'trading'
                AND tc.table_name = t.table_name
        ) THEN 'YES'
        ELSE 'NO'
    END AS is_primary_key,
    CASE 
        WHEN c.column_name IN (
            SELECT kcu.column_name 
            FROM information_schema.table_constraints tc
            JOIN information_schema.key_column_usage kcu 
                ON tc.constraint_name = kcu.constraint_name
                AND tc.table_schema = kcu.table_schema
            WHERE tc.constraint_type = 'FOREIGN KEY'
                AND tc.table_schema = 'trading'
                AND tc.table_name = t.table_name
        ) THEN 'YES'
        ELSE 'NO'
    END AS is_foreign_key,
    col_description(pgc.oid, c.ordinal_position) AS column_comment
FROM information_schema.tables t
JOIN information_schema.columns c 
    ON t.table_name = c.table_name 
    AND t.table_schema = c.table_schema
JOIN pg_class pgc 
    ON pgc.relname = t.table_name
WHERE t.table_schema = 'trading'
ORDER BY t.table_name, c.ordinal_position;

-- 3. Show table constraints (primary keys, foreign keys, unique constraints)
SELECT 
    tc.table_name,
    tc.constraint_name,
    tc.constraint_type,
    kcu.column_name,
    ccu.table_name AS foreign_table_name,
    ccu.column_name AS foreign_column_name
FROM information_schema.table_constraints tc
LEFT JOIN information_schema.key_column_usage kcu 
    ON tc.constraint_name = kcu.constraint_name
    AND tc.table_schema = kcu.table_schema
LEFT JOIN information_schema.constraint_column_usage ccu 
    ON tc.constraint_name = ccu.constraint_name
    AND tc.table_schema = ccu.constraint_schema
WHERE tc.table_schema = 'trading'
ORDER BY tc.table_name, tc.constraint_type, kcu.column_name;

-- 4. Show indexes on trading schema tables
SELECT 
    schemaname,
    tablename,
    indexname,
    indexdef
FROM pg_indexes 
WHERE schemaname = 'trading'
ORDER BY tablename, indexname;

-- 5. Show table sizes and row counts (approximate)
SELECT 
    schemaname,
    tablename,
    pg_size_pretty(pg_total_relation_size(schemaname||'.'||tablename)) AS size,
    n_tup_ins AS inserts,
    n_tup_upd AS updates,
    n_tup_del AS deletes,
    n_live_tup AS live_rows,
    n_dead_tup AS dead_rows
FROM pg_stat_user_tables 
WHERE schemaname = 'trading'
ORDER BY pg_total_relation_size(schemaname||'.'||tablename) DESC;

-- 6. Simple summary view - just table names and column counts
SELECT 
    t.table_name,
    COUNT(c.column_name) AS column_count,
    string_agg(c.column_name || ' (' || c.data_type || ')', ', ' ORDER BY c.ordinal_position) AS columns
FROM information_schema.tables t
LEFT JOIN information_schema.columns c 
    ON t.table_name = c.table_name 
    AND t.table_schema = c.table_schema
WHERE t.table_schema = 'trading'
GROUP BY t.table_name
ORDER BY t.table_name;
