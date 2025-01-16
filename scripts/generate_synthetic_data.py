#!/usr/bin/env python3

import os
import sys
from datetime import datetime, timedelta, timezone
import pandas as pd
import numpy as np
import psycopg2
from psycopg2.extras import execute_values
import logging

# Set up logging
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')

def get_db_connection():
    logging.info("Connecting to database...")
    return psycopg2.connect(
        host=os.getenv('DB_HOST', '3.140.200.228'),
        port=os.getenv('DB_PORT', '5432'),
        database=os.getenv('DB_NAME', 'algo_data'),
        user=os.getenv('DB_USER', 'postgres'),
        password=os.getenv('DB_PASSWORD', 'algogators')
    )

def ensure_schema_exists(conn):
    logging.info("Ensuring schema and table exist...")
    with conn.cursor() as cur:
        cur.execute("""
        CREATE SCHEMA IF NOT EXISTS futures_data;
        """)
        
        cur.execute("""
        CREATE TABLE IF NOT EXISTS futures_data.ohlcv_1d (
            time TIMESTAMPTZ NOT NULL,
            symbol TEXT NOT NULL,
            open DOUBLE PRECISION NOT NULL,
            high DOUBLE PRECISION NOT NULL,
            low DOUBLE PRECISION NOT NULL,
            close DOUBLE PRECISION NOT NULL,
            volume INTEGER NOT NULL,
            PRIMARY KEY (time, symbol)
        );
        """)
        conn.commit()
    logging.info("Schema and table ready")

def generate_synthetic_data(symbol="ES", start_date="2024-01-01", end_date="2024-01-10", base_price=4700):
    """Generate synthetic OHLCV data with realistic price movements."""
    start = pd.Timestamp(start_date)
    end = pd.Timestamp(end_date)
    dates = pd.date_range(start=start, end=end, freq='B')  # Business days only
    
    data = []
    current_price = base_price
    
    for date in dates:
        # Generate daily volatility (0.5% to 1.5% of price)
        daily_volatility = current_price * np.random.uniform(0.005, 0.015)
        
        # Generate OHLCV data
        open_price = current_price
        high_price = open_price + abs(np.random.normal(0, daily_volatility))
        low_price = open_price - abs(np.random.normal(0, daily_volatility))
        close_price = np.random.uniform(low_price, high_price)
        
        # Generate volume (between 100k and 500k contracts)
        volume = int(np.random.uniform(100000, 500000))
        
        data.append({
            'time': date.replace(tzinfo=timezone.utc),
            'symbol': symbol,
            'open': round(open_price, 2),
            'high': round(high_price, 2),
            'low': round(low_price, 2),
            'close': round(close_price, 2),
            'volume': volume
        })
        
        # Update current price for next day
        current_price = close_price
    
    return pd.DataFrame(data)

def insert_batch(conn, records):
    """Insert a batch of records into the database."""
    with conn.cursor() as cur:
        execute_values(
            cur,
            """
            INSERT INTO futures_data.ohlcv_1d (time, symbol, open, high, low, close, volume)
            VALUES %s
            ON CONFLICT (time, symbol) DO UPDATE SET
                open = EXCLUDED.open,
                high = EXCLUDED.high,
                low = EXCLUDED.low,
                close = EXCLUDED.close,
                volume = EXCLUDED.volume;
            """,
            records
        )
    conn.commit()

def load_synthetic_data():
    logging.info("Generating synthetic data...")
    
    # Generate data for multiple symbols
    symbols = {
        'ES': 4700,  # E-mini S&P 500
        'NQ': 16500, # E-mini NASDAQ
        'YM': 37500, # E-mini Dow
        'RTY': 2100, # E-mini Russell 2000
        'CL': 72,    # Crude Oil
    }
    
    try:
        conn = get_db_connection()
        ensure_schema_exists(conn)
        
        total_records = 0
        for symbol, base_price in symbols.items():
            try:
                df = generate_synthetic_data(symbol=symbol, base_price=base_price)
                logging.info(f"Generated {len(df)} records for {symbol}")
                
                # Convert DataFrame rows to records
                records = []
                for _, row in df.iterrows():
                    records.append((
                        row['time'],
                        row['symbol'],
                        row['open'],
                        row['high'],
                        row['low'],
                        row['close'],
                        row['volume']
                    ))
                
                # Insert records
                insert_batch(conn, records)
                total_records += len(records)
                logging.info(f"Successfully loaded {len(records)} records for {symbol}")
                
            except Exception as e:
                logging.error(f"Error processing symbol {symbol}: {e}")
                continue
        
        logging.info(f"Total records loaded: {total_records}")
        
    except Exception as e:
        logging.error(f"Error loading synthetic data: {e}")
    finally:
        if conn:
            conn.close()
            logging.info("Database connection closed")

if __name__ == "__main__":
    load_synthetic_data() 