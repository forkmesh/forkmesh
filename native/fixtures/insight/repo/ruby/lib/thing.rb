module Thing
  def self.call(value)
    case value
    when Integer then value * 2
    when String then value.upcase
    else nil
    end
  end
end
