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

def fetch_and_load_data(api_key, start_date="2024-01-01", end_date="2024-01-31"):
    # Initialize Databento client
    logging.info("Initializing Databento client...")
    client = Historical(api_key)
    
    try:
        # First, get metadata about available data
        metadata = client.metadata.list_datasets()
        logging.info(f"Available datasets: {metadata}")
        
        # Get available symbols
        for dataset in metadata:
            logging.info(f"\nChecking symbols in dataset: {dataset}")
            try:
                symbols = client.metadata.list_symbols(dataset=dataset)
                logging.info(f"Found {len(symbols)} symbols")
                if symbols:
                    logging.info(f"Sample symbols: {symbols[:5]}")
                    
                    # Try to get data for the first symbol
                    symbol = symbols[0]
                    logging.info(f"\nFetching data for {symbol} from {dataset}")
                    
                    df = client.timeseries.get_range(
                        dataset=dataset,
                        symbols=[symbol],
                        start=start_date,
                        end=end_date,
                        schema="ohlcv"
                    ).to_df()
                    
                    if df is not None and not df.empty:
                        logging.info(f"Received data shape: {df.shape}")
                        logging.info(f"Sample data:\n{df.head()}")
                        logging.info(f"Columns: {df.columns}")
                        
                        # Connect to PostgreSQL
                        conn = get_db_connection()
                        ensure_schema_exists(conn)
                        
                        # Prepare data for insertion
                        records = []
                        for _, row in df.iterrows():
                            records.append((
                                pd.Timestamp(row['ts_event']).tz_localize(timezone.utc),
                                symbol,
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
                        conn.close()
                        return  # Exit after successfully loading data
                    
            except Exception as e:
                logging.error(f"Error with dataset {dataset}: {e}")
                continue
    
    except Exception as e:
        logging.error(f"Error fetching data from Databento: {e}")

if __name__ == "__main__":
    api_key = os.getenv('DATABENTO_API_KEY')
    if not api_key:
        logging.error("Error: DATABENTO_API_KEY environment variable not set")
        sys.exit(1)
    
    fetch_and_load_data(api_key) 