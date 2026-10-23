"""
forecast.py
LibraryDeskSense - Forecasting module
Reads noise/light data from InfluxDB, fits an ARIMA model,
forecasts future values, evaluates accuracy (MAE / MSE),
and writes forecasts back to InfluxDB for Grafana.
"""

import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import time
import warnings
warnings.filterwarnings("ignore")
from datetime import datetime, timedelta, timezone
from influxdb_client import InfluxDBClient, Point
from influxdb_client.client.write_api import SYNCHRONOUS
from statsmodels.tsa.arima.model import ARIMA
from sklearn.metrics import mean_absolute_error, mean_squared_error

# Configuration 
INFLUX_URL = "http://localhost:8086"
INFLUX_TOKEN = "n3dZ8vwowLKX80Z8nGKHGGSMMxeFBa8rZeJ3YGev8yGD9FYbBhi5QTnF69clJafTryoGVpusOazurdP8Mh9R1g=="
INFLUX_ORG = "LibraryDeskSense"
INFLUX_BUCKET = "library_desk"

FIELD_TO_FORECAST = "noise"
HISTORY_MINUTES = 30
FORECAST_STEPS = 300
TRAIN_RATIO = 0.8
ARIMA_ORDER = (2, 1, 2)
RUN_INTERVAL_SECONDS = 60

# ===== Section 3: Read Data from InfluxDB =====
def read_data_from_influx(field_name, minutes):
    """Query the last N minutes of a field and return a pandas DataFrame."""
    client = InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)
    query_api = client.query_api()

    flux_query = f'''
    from(bucket: "{INFLUX_BUCKET}")
      |> range(start: -{minutes}m)
      |> filter(fn: (r) => r._measurement == "telemetry")
      |> filter(fn: (r) => r._field == "{field_name}")
      |> keep(columns: ["_time", "_value"])
    '''

    tables = query_api.query(flux_query)

    times = []
    values = []
    for table in tables:
        for record in table.records:
            times.append(record.get_time())
            values.append(record.get_value())

    client.close()

    df = pd.DataFrame({"time": times, "value": values})
    df = df.set_index("time").sort_index()
    return df

# Section 4: Fit ARIMA, Forecast, Evaluate 
def fit_arima_and_forecast(df, order, train_ratio, forecast_steps):
    """Split data, fit ARIMA on train set, evaluate on test set,
    then forecast future steps. Returns (fitted_model, test_actual, test_predicted, future_forecast)."""
    series = df["value"].astype(float)

    split_index = int(len(series) * train_ratio)
    train = series.iloc[:split_index]
    test = series.iloc[split_index:]

    model = ARIMA(train, order=order)
    fitted = model.fit()

    test_predicted = fitted.forecast(steps=len(test))

    mae = mean_absolute_error(test, test_predicted)
    mse = mean_squared_error(test, test_predicted)
    print(f"Evaluation on test set:")
    print(f"  MAE = {mae:.3f}")
    print(f"  MSE = {mse:.3f}")

    full_model = ARIMA(series, order=order).fit()
    future_forecast = full_model.forecast(steps=forecast_steps)

    return fitted, test, test_predicted, future_forecast, mae, mse

# Section 5: Plot the Results 
# def plot_results(df, train_ratio, test_actual, test_predicted, future_forecast, field_name):
#   """Plot training data, test actual vs predicted, and future forecast."""
#   series = df["value"].astype(float)
#    split_index = int(len(series) * train_ratio)
#    train = series.iloc[:split_index]
#
#   plt.figure(figsize=(12, 6))
#    plt.plot(train.index, train.values, label="Training data", color="blue")
#    plt.plot(test_actual.index, test_actual.values, label="Test actual", color="green")
#    plt.plot(test_actual.index, test_predicted.values, label="Test predicted", color="orange", linestyle="--")

#    last_time = series.index[-1]
#    future_times = pd.date_range(start=last_time, periods=len(future_forecast) + 1, freq="1s")[1:]
#    plt.plot(future_times, future_forecast.values, label="Future forecast", color="red", linestyle="--")

#    plt.title(f"ARIMA Forecast - {field_name}")
#    plt.xlabel("Time")
#    plt.ylabel(field_name)
#    plt.legend()
#    plt.grid(True)
#    plt.tight_layout()

#    filename = f"forecast_{field_name}.png"
#    plt.savefig(filename)
#    print(f"Plot saved to {filename}")
#    plt.show() 


# Section 6: Write Forecast to InfluxDB 
def write_forecast_to_influx(future_forecast, field_name):
    """Write forecasted values to InfluxDB as a separate measurement."""
    client = InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)

    delete_api = client.delete_api()
    delete_api.delete(
        start=datetime(1970, 1, 1, tzinfo=timezone.utc),
        stop=datetime(2100, 1, 1, tzinfo=timezone.utc),
        predicate='_measurement="forecast"',
        bucket=INFLUX_BUCKET,
        org=INFLUX_ORG
    )

    write_api = client.write_api(write_options=SYNCHRONOUS)

    now = datetime.now(timezone.utc)
    points = []

    for i, value in enumerate(future_forecast.values):
        future_time = now + timedelta(seconds=i + 1)
        point = (
            Point("forecast")
            .tag("field", field_name)
            .field("predicted_value", float(value))
            .time(future_time)
        )
        points.append(point)

    write_api.write(bucket=INFLUX_BUCKET, org=INFLUX_ORG, record=points)
    client.close()

    print(f"Wrote {len(points)} forecast points to InfluxDB (measurement=forecast, field={field_name})")

if __name__ == "__main__":
    print(f"Forecast service starting. Runs every {RUN_INTERVAL_SECONDS}s. Ctrl+C to stop.")
    while True:
        try:
            print(f"\n=== New forecast cycle at {datetime.now().strftime('%H:%M:%S')} ===")
            df = read_data_from_influx(FIELD_TO_FORECAST, HISTORY_MINUTES)
            print(f"Got {len(df)} rows")

            if len(df) < 50:
                print("Not enough data yet, skipping this cycle.")
            else:
                fitted, test, test_pred, future, mae, mse = fit_arima_and_forecast(
                    df, ARIMA_ORDER, TRAIN_RATIO, FORECAST_STEPS
                )
                write_forecast_to_influx(future, FIELD_TO_FORECAST)

            print(f"Sleeping {RUN_INTERVAL_SECONDS}s...")
            time.sleep(RUN_INTERVAL_SECONDS)
        except KeyboardInterrupt:
            print("\nStopped by user.")
            break
        except Exception as e:
            print(f"Error: {e}")
            time.sleep(RUN_INTERVAL_SECONDS)




    