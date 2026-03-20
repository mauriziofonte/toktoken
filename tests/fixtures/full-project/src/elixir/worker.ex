defmodule Worker do
  use GenServer

  @default_interval 5_000
  @max_retries 3

  defstruct [:name, :task, :interval, retries: 0, status: :idle]

  def start_link(opts \\ []) do
    name = Keyword.get(opts, :name, __MODULE__)
    task = Keyword.get(opts, :task, &default_task/1)
    interval = Keyword.get(opts, :interval, @default_interval)

    state = %__MODULE__{
      name: name,
      task: task,
      interval: interval
    }

    GenServer.start_link(__MODULE__, state, name: name)
  end

  def get_status(pid) do
    GenServer.call(pid, :get_status)
  end

  def reset(pid) do
    GenServer.cast(pid, :reset)
  end

  @impl true
  def init(%__MODULE__{} = state) do
    schedule_work(state.interval)
    {:ok, %{state | status: :running}}
  end

  @impl true
  def handle_call(:get_status, _from, state) do
    reply = %{
      name: state.name,
      status: state.status,
      retries: state.retries
    }
    {:reply, reply, state}
  end

  @impl true
  def handle_cast(:reset, state) do
    {:noreply, %{state | retries: 0, status: :running}}
  end

  @impl true
  def handle_info(:perform_work, %{status: :stopped} = state) do
    {:noreply, state}
  end

  def handle_info(:perform_work, state) do
    case execute_task(state) do
      {:ok, _result} ->
        schedule_work(state.interval)
        {:noreply, %{state | retries: 0, status: :running}}

      {:error, reason} when state.retries < @max_retries ->
        schedule_work(state.interval * 2)
        {:noreply, %{state | retries: state.retries + 1}}

      {:error, _reason} ->
        {:noreply, %{state | status: :stopped}}
    end
  end

  defp execute_task(%{task: task, name: name}) do
    try do
      result = task.(name)
      {:ok, result}
    rescue
      exception -> {:error, Exception.message(exception)}
    end
  end

  defp schedule_work(interval) do
    Process.send_after(self(), :perform_work, interval)
  end

  defp default_task(name) do
    IO.puts("[#{name}] Performing scheduled work at #{DateTime.utc_now()}")
    :ok
  end
end
