#!/usr/bin/env python3

import os
import logging
from databento import Historical

# Set up logging
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')

def test_connection(api_key):
    logging.info(f"Testing connection with API key: {api_key}")
    try:
        client = Historical(api_key)
        logging.info("Successfully initialized client")
        
        # Try to list datasets
        datasets = client.metadata.list_datasets()
        logging.info(f"Successfully retrieved datasets: {datasets}")
        
    except Exception as e:
        logging.error(f"Error connecting to Databento: {str(e)}")

if __name__ == "__main__":
    api_key = os.getenv('DATABENTO_API_KEY')
    if not api_key:
        logging.error("Error: DATABENTO_API_KEY environment variable not set")
        exit(1)
        
    test_connection(api_key) 