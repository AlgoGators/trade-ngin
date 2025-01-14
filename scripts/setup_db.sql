-- Create tables
CREATE TABLE IF NOT EXISTS contracts (
    id SERIAL PRIMARY KEY,
    symbol VARCHAR(10) NOT NULL,
    exchange VARCHAR(10) NOT NULL,
    contract_date DATE NOT NULL,
    open DECIMAL(10,2),
    high DECIMAL(10,2),
    low DECIMAL(10,2),
    close DECIMAL(10,2),
    volume INTEGER,
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    UNIQUE(symbol, exchange, contract_date)
);

-- Create indexes
CREATE INDEX IF NOT EXISTS idx_contracts_symbol ON contracts(symbol);
CREATE INDEX IF NOT EXISTS idx_contracts_date ON contracts(contract_date);

-- Insert sample data for ES (E-mini S&P 500)
INSERT INTO contracts (symbol, exchange, contract_date, open, high, low, close, volume)
VALUES 
    ('ES', 'CME', '2023-01-03', 3824.25, 3892.75, 3822.00, 3888.75, 2145367),
    ('ES', 'CME', '2023-01-04', 3889.50, 3907.00, 3838.25, 3852.50, 1987654),
    ('ES', 'CME', '2023-01-05', 3853.00, 3863.75, 3802.25, 3822.75, 1876543),
    ('ES', 'CME', '2023-01-06', 3823.00, 3907.25, 3809.50, 3895.00, 2098765),
    ('ES', 'CME', '2023-01-09', 3895.25, 3950.75, 3890.50, 3945.25, 1987654)
ON CONFLICT (symbol, exchange, contract_date) DO NOTHING; 