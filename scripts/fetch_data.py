#!/usr/bin/env python3

import os
import sys
from datetime import datetime
import psycopg2
from databento import Historical
import logging

# Set up logging
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')

def get_db_connection():
    """Create a connection to the PostgreSQL database."""
    try:
        conn = psycopg2.connect(
            host=os.getenv("DB_HOST", "3.140.200.228"),
            port=os.getenv("DB_PORT", "5432"),
            database=os.getenv("DB_NAME", "algo_data"),
            user=os.getenv("DB_USER", "postgres"),
            password=os.getenv("DB_PASSWORD", "algogators")
        )
        logging.info("Successfully connected to the database")
        return conn
    except Exception as e:
        logging.error(f"Failed to connect to database: {e}")
        raise

def ensure_schema_exists(conn):
    """Ensure the futures_data schema and table exist."""
    try:
        with conn.cursor() as cur:
            logging.info("Creating schema if it doesn't exist...")
            cur.execute("CREATE SCHEMA IF NOT EXISTS futures_data;")
            
            logging.info("Creating table if it doesn't exist...")
            cur.execute("""
                CREATE TABLE IF NOT EXISTS futures_data.ohlcv_1d (
                    time TIMESTAMP WITH TIME ZONE NOT NULL,
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
            logging.info("Schema and table are ready")
    except Exception as e:
        logging.error(f"Failed to create schema/table: {e}")
        raise

def fetch_and_load_data(api_key, symbol="ES", start_date="2023-01-01", end_date="2024-01-09"):
    """Fetch daily OHLCV data from Databento and load it into the database."""
    conn = None
    try:
        # Initialize Databento client
        logging.info(f"Initializing Databento client with API key: {api_key[:8]}...")
        client = Historical(api_key)
        
        # Get database connection and ensure schema exists
        conn = get_db_connection()
        ensure_schema_exists(conn)
        
        # Fetch data from Databento
        logging.info(f"Fetching data for {symbol} from {start_date} to {end_date}")
        try:
            df = client.get_historical_ohlcv(
                dataset="CME",
                symbols=[symbol],
                start=start_date,
                end=end_date,
                interval="1d"
            )
            logging.info(f"Received DataFrame with shape: {df.shape if df is not None else 'None'}")
        except Exception as e:
            logging.error(f"Failed to fetch data from Databento: {e}")
            raise
        
        if df is None or df.empty:
            logging.warning("No data found for the specified parameters")
            return
        
        # Insert data into the database
        with conn.cursor() as cur:
            rows_inserted = 0
            for _, row in df.iterrows():
                try:
                    cur.execute("""
                        INSERT INTO futures_data.ohlcv_1d 
                        (time, symbol, open, high, low, close, volume)
                        VALUES (%s, %s, %s, %s, %s, %s, %s)
                        ON CONFLICT (time, symbol) DO UPDATE SET
                        open = EXCLUDED.open,
                        high = EXCLUDED.high,
                        low = EXCLUDED.low,
                        close = EXCLUDED.close,
                        volume = EXCLUDED.volume;
                    """, (
                        row.timestamp,
                        symbol,
                        row.open,
                        row.high,
                        row.low,
                        row.close,
                        row.volume
                    ))
                    rows_inserted += 1
                except Exception as e:
                    logging.error(f"Failed to insert row: {row}\nError: {e}")
                    continue
            
            conn.commit()
            logging.info(f"Successfully inserted/updated {rows_inserted} rows")
            
    except Exception as e:
        logging.error(f"Error in fetch_and_load_data: {e}")
        if conn:
            conn.rollback()
        sys.exit(1)
    finally:
        if conn:
            conn.close()
            logging.info("Database connection closed")

if __name__ == "__main__":
    api_key = os.getenv("DATABENTO_API_KEY")
    if not api_key:
        logging.error("Error: DATABENTO_API_KEY environment variable not set")
        sys.exit(1)
        
    fetch_and_load_data(api_key) 