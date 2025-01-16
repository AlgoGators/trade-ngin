#!/usr/bin/env python3

import os
import sys
from datetime import datetime, timedelta, timezone
import random
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

def generate_ohlcv_data():
    """Generate 10 days of synthetic OHLCV data for ES."""
    data = []
    base_price = 4700
    current_price = base_price
    
    # Generate data for the last 10 business days
    end_date = datetime.now(timezone.utc).replace(hour=0, minute=0, second=0, microsecond=0)
    dates = []
    current_date = end_date
    
    # Get 10 business days
    while len(dates) < 10:
        if current_date.weekday() < 5:  # Monday = 0, Friday = 4
            dates.append(current_date)
        current_date = current_date - timedelta(days=1)
    
    # Reverse dates to go forward in time
    dates = dates[::-1]
    
    for date in dates:
        # Generate daily price movement (±1%)
        price_change = current_price * random.uniform(-0.01, 0.01)
        
        open_price = current_price
        close_price = current_price + price_change
        high_price = max(open_price, close_price) + abs(random.uniform(0, price_change))
        low_price = min(open_price, close_price) - abs(random.uniform(0, price_change))
        volume = random.randint(100000, 500000)
        
        data.append((
            date,
            'ES',
            round(open_price, 2),
            round(high_price, 2),
            round(low_price, 2),
            round(close_price, 2),
            volume
        ))
        
        current_price = close_price
    
    return data

def load_synthetic_data():
    logging.info("Generating synthetic data...")
    
    try:
        conn = get_db_connection()
        ensure_schema_exists(conn)
        
        # Generate and insert data
        records = generate_ohlcv_data()
        logging.info(f"Generated {len(records)} records")
        
        # Print records for verification
        for record in records:
            logging.info(f"Record: {record}")
        
        # Insert records
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
        logging.info(f"Successfully loaded {len(records)} records")
        
    except Exception as e:
        logging.error(f"Error loading synthetic data: {e}")
    finally:
        if conn:
            conn.close()
            logging.info("Database connection closed")

if __name__ == "__main__":
    load_synthetic_data() 