module Helpers
  def format_output(data)
    case data
    when Hash
      data.map { |k, v| "#{k}=#{v}" }.join(', ')
    when Array
      data.map(&:to_s).join(' | ')
    else
      data.to_s.strip
    end
  end

  def validate_input(value, pattern: /\S+/, message: nil)
    str = value.to_s.strip
    if str.empty?
      raise ArgumentError, message || 'Input must not be empty'
    end

    unless str.match?(pattern)
      raise ArgumentError, message || "Input '#{str}' does not match #{pattern.inspect}"
    end

    str
  end

  def log_message(msg, level: :info, timestamp: true)
    prefix = timestamp ? "[#{Time.now.strftime('%Y-%m-%d %H:%M:%S')}]" : ''
    tag = level.to_s.upcase.ljust(5)
    formatted = "#{prefix} [#{tag}] #{msg}"
    $stderr.puts(formatted) if level == :error
    $stdout.puts(formatted) unless level == :error
    formatted
  end
end

include Helpers
