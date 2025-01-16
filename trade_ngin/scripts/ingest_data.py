#!/usr/bin/env python3

import os
import sys
from datetime import datetime, timezone
import pandas as pd
from databento import Historical
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

def fetch_and_load_data(api_key):
    # Initialize Databento client
    logging.info("Initializing Databento client...")
    client = Historical(api_key)
    
    # Get daily OHLCV data
    try:
        df = client.timeseries.get_range(
            dataset="GLBX.MDP3",
            symbols=["ES"],
            start="2024-01-01",
            end="2024-01-31",
            schema="ohlcv-1d"
        ).to_df()
        logging.info(f"Received data shape: {df.shape}")
        logging.info(f"Sample data:\n{df.head()}")
    except Exception as e:
        logging.error(f"Error fetching data from Databento: {e}")
        return
    
    if df.empty:
        logging.warning("No data returned from Databento")
        return
    
    # Connect to PostgreSQL
    try:
        conn = get_db_connection()
        ensure_schema_exists(conn)
        
        # Prepare data for insertion
        records = []
        for _, row in df.iterrows():
            records.append((
                pd.Timestamp(row['ts_event']).tz_localize(timezone.utc),
                "ES",
                row['open'],
                row['high'],
                row['low'],
                row['close'],
                row['volume']
            ))
        
        logging.info(f"Preparing to insert {len(records)} records")
        
        # Insert data
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
        logging.error(f"Error loading data to PostgreSQL: {e}")
    finally:
        if conn:
            conn.close()
            logging.info("Database connection closed")

if __name__ == "__main__":
    api_key = os.getenv('DATABENTO_API_KEY')
    if not api_key:
        logging.error("Error: DATABENTO_API_KEY environment variable not set")
        sys.exit(1)
    
    fetch_and_load_data(api_key) 