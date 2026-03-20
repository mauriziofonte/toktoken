require_relative './helpers'

class Application
  attr_accessor :name, :config, :running

  def initialize(name, options = {})
    @name = name
    @config = {
      log_level: options.fetch(:log_level, :info),
      max_retries: options.fetch(:max_retries, 3),
      timeout: options.fetch(:timeout, 30)
    }
    @running = false
    @start_time = nil
  end

  def run
    configure
    @running = true
    @start_time = Time.now
    log_message("Application '#{@name}' started at #{@start_time}")

    begin
      yield self if block_given?
    rescue StandardError => e
      log_message("Error during execution: #{e.message}", level: :error)
      raise
    ensure
      shutdown
    end
  end

  def configure
    validate_input(@name, pattern: /\A[a-zA-Z0-9_-]+\z/)
    log_message("Configuration loaded: #{format_output(@config)}")
  end

  def shutdown
    return unless @running

    elapsed = @start_time ? Time.now - @start_time : 0
    @running = false
    log_message("Application '#{@name}' stopped after #{elapsed.round(2)}s")
  end

  def uptime
    return 0 unless @running && @start_time
    Time.now - @start_time
  end
end
